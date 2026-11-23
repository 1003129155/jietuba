#include "translation/line_grouping.h"
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace jietuba {
namespace {
// Decision trees trained on screenshots whose paragraphs are known. They score a pair of lines,
// one above the other, from measurements that do not depend on the font size; a positive score
// says the lower line continues the upper one.
// A child index at or above zero is another split, below zero the leaf ~index.
struct ModelSplit {
    float threshold;
    short left;
    short right;
    unsigned char feature;
};
#include "translation/line_grouping_model.inc"
static_assert(MODEL_FEATURE_COUNT == PARAGRAPH_FEATURE_COUNT, "the model was trained on another set of features");

// Longest paragraph kept: a run of unrelated lines that happens to line up stays readable.
constexpr int MAX_PARAGRAPH_LINES = 30;
const QString STOPS = QString::fromUtf8(".!?。！？");
const QString COMMAS = QString::fromUtf8(",，、;；");
const QString COLONS = QString::fromUtf8(":：");
const QString LABEL_ENDS = QString::fromUtf8(".!?:-。！？：");

// Every feature is computed the same way as by the training script, operation for operation, so
// the trees see the values they were trained on.
struct Box {
    double x0, y0, x1, y1;
};

double median_of(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const size_t count = values.size();
    return count % 2 ? values[count / 2] : (values[count / 2 - 1] + values[count / 2]) / 2;
}

double percentile30(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const double k = static_cast<double>(values.size() - 1) * 0.3;
    const auto floor = static_cast<size_t>(std::floor(k));
    const size_t ceil = std::min(floor + 1, values.size() - 1);
    return values[floor] + (values[ceil] - values[floor]) * (k - static_cast<double>(floor));
}

QVector<QVector<int>> components(int count, const std::vector<std::pair<int, int>> &edges) {
    std::vector<int> parent(static_cast<size_t>(count));
    std::iota(parent.begin(), parent.end(), 0);
    const auto find = [&parent](int index) {
        while (parent[static_cast<size_t>(index)] != index) {
            parent[static_cast<size_t>(index)] = parent[static_cast<size_t>(parent[static_cast<size_t>(index)])];
            index = parent[static_cast<size_t>(index)];
        }
        return index;
    };
    for (const auto &[a, b] : edges) parent[static_cast<size_t>(find(a))] = find(b);
    QVector<QVector<int>> groups;
    std::vector<int> slot(static_cast<size_t>(count), -1);
    for (int index = 0; index < count; ++index) {
        int &place = slot[static_cast<size_t>(find(index))];
        if (place < 0) {
            place = static_cast<int>(groups.size());
            groups.append(QVector<int>());
        }
        groups[place].append(index);
    }
    return groups;
}

// List markers other than numbers: bullets, circled numbers, "一、", "(가)". A dash needs a space or
// a CJK character after it, and "o"/"O" (a hollow bullet read as a letter) a space and a capital or
// CJK character.
#define LIST_BULLETS "\\x{2022}\\x{25CF}\\x{25A0}\\x{25AA}\\x{25CB}\\x{25E6}\\x{25C6}\\x{25C7}\\x{25A1}\\x{25B6}\\x{25BA}\\x{25B8}\\x{27A2}\\x{2713}\\x{2714}\\x{2605}\\x{2606}\\x{203B}\\x{30FB}"
#define LIST_CJK "\\x{3040}-\\x{30FF}\\x{3400}-\\x{9FFF}\\x{AC00}-\\x{D7AF}"
#define LIST_HAN_NUMERALS "\\x{4E00}\\x{4E8C}\\x{4E09}\\x{56DB}\\x{4E94}\\x{516D}\\x{4E03}\\x{516B}\\x{4E5D}\\x{5341}"
#define LIST_NUMBER "[(\\x{FF08}]?\\d{1,3}[.)\\x{3001}\\x{FF0E}\\x{FF09}]"
#define LIST_OTHER_MARKERS                                                                                                                          \
    "[" LIST_BULLETS "\\x{2460}-\\x{2473}]\\s*\\S|[" LIST_HAN_NUMERALS "]{1,3}\\x{3001}\\s*\\S|\\([\\x{AC00}-\\x{D7A3}]\\)\\s*\\S|[-*\\x{00B7}\\x{2013}]\\s+\\S|" \
    "[-*\\x{2013}][" LIST_CJK "]|[oO]\\s+[\\p{Lu}" LIST_CJK "]"

// "1." "2)" "3、", a bullet or another marker: an item of a list. For the trees also "[12]" and ". "
// (a small bullet read as a dot), which a screenshot cutting a line in two can leave at its start.
bool starts_list_item(const QString &text) {
    static const QRegularExpression marker(QStringLiteral("^\\s*(?:" LIST_NUMBER "\\s*\\S|\\[\\d{1,4}\\]\\s*\\S|\\.\\s+\\S|" LIST_OTHER_MARKERS ")"));
    return marker.match(text).hasMatch();
}

// A marker with nothing after it: a bullet or a list number recognized as a box of its own.
bool is_marker_only(const QString &text) {
    static const QRegularExpression marker(QStringLiteral("^\\s*(?:[" LIST_BULLETS "\\x{2460}-\\x{2473}\\x{00B7}oO]|" LIST_NUMBER "|[" LIST_HAN_NUMERALS
                                                          "]{1,3}[.\\x{3001}\\x{FF0E}]|\\([\\x{AC00}-\\x{D7A3}]\\))\\s*$"));
    return marker.match(text).hasMatch();
}

double cjk_share(const QString &text) {
    qsizetype count = 0;
    for (const QChar c : text) count += is_cjk(c) ? 1 : 0;
    return static_cast<double>(count) / static_cast<double>(std::max<qsizetype>(1, text.size()));
}

bool ends_in(const QString &text, const QString &chars) { return !text.isEmpty() && chars.contains(text.back()); }

// A short, capitalised line that does not end a sentence: a menu entry or a field label.
bool looks_like_short_label(const QString &text, const Box &box) {
    if (text.isEmpty() || (box.x1 - box.x0) > (box.y1 - box.y0) * 8 || LABEL_ENDS.contains(text.back()) || starts_list_item(text)) return false;
    // Five or six characters is already a wrapped line of a narrow table cell.
    if (is_cjk(text.front())) return text.size() <= 4;
    return text.front().isUpper() && text.size() <= 24 && text.count(u' ') <= 2;
}

// b continues a's column: overlaps it, lies within 1.2 line heights above or below, is about as
// tall, and is flush with it on one side or centred on it.
bool loosely_stacked(const Box &a, const Box &b) {
    const double height = a.y1 - a.y0, reach = 1.2 * height, tolerance = 0.5 * height, other = b.y1 - b.y0;
    if (!(b.x1 > a.x0 && b.x0 < a.x1 && b.y1 > a.y0 - reach && b.y0 < a.y1 + reach)) return false;
    if (std::abs(other - height) > tolerance || std::max(other, height) / std::min(other, height) >= 1.5) return false;
    return std::abs(b.x0 - a.x0) <= tolerance || std::abs(b.x1 - a.x1) <= tolerance || std::abs((b.x0 + b.x1) / 2 - (a.x0 + a.x1) / 2) <= tolerance;
}

double score(const std::array<float, PARAGRAPH_FEATURE_COUNT> &features) {
    double raw = MODEL_BASELINE;
    for (const short root : MODEL_TREES) {
        int node = root;
        while (node >= 0) {
            const auto &split = MODEL_SPLITS[node];
            node = features[split.feature] <= split.threshold ? split.left : split.right;
        }
        raw += MODEL_LEAVES[~node];
    }
    return raw;
}
}

bool is_cjk(QChar c) {
    const auto u = c.unicode();
    return (u >= 0x3000 && u <= 0x30FF) || (u >= 0x3400 && u <= 0x9FFF) || (u >= 0xAC00 && u <= 0xD7AF) || (u >= 0xF900 && u <= 0xFAFF) ||
           (u >= 0xFF00 && u <= 0xFFEF);
}

bool opens_list_item(const QString &text) {
    static const QRegularExpression marker(QStringLiteral("^\\s*(?:" LIST_NUMBER "\\s*[^\\d\\s]|" LIST_OTHER_MARKERS ")"));
    return marker.match(text).hasMatch();
}

QVector<int> detached_list_markers(const QVector<GroupingLine> &lines) {
    QVector<int> owner(lines.size(), -1);
    QVector<bool> marker(lines.size());
    for (int i = 0; i < lines.size(); ++i) marker[i] = is_marker_only(lines[i].text);
    for (int m = 0; m < lines.size(); ++m) {
        if (!marker[m]) continue;
        const auto &mark = lines[m].rect;
        double best = 0;
        for (int t = 0; t < lines.size(); ++t) {
            const auto &item = lines[t].rect;
            if (marker[t] || mark.center().y() < item.top() || mark.center().y() > item.bottom() || item.left() < mark.center().x() ||
                mark.height() > 1.5 * item.height())
                continue;
            // Within a line height: a cell of the next table column or an equation number lies further off.
            const double gap = (item.left() - mark.right()) / item.height();
            if (gap <= 1.0 && (owner[m] < 0 || gap < best)) {
                owner[m] = t;
                best = gap;
            }
        }
    }
    return owner;
}

bool is_short_label(const GroupingLine &line) {
    return looks_like_short_label(line.text, {line.rect.left(), line.rect.top(), line.rect.right(), line.rect.bottom()});
}

const std::array<const char *, PARAGRAPH_FEATURE_COUNT> PARAGRAPH_FEATURE_NAMES = {
    "gap", "pitch", "pitch_page", "pitch_col", "dl", "dr", "dc", "wa", "wb", "wratio", "ovl", "hratio", "fill_a", "fill_b", "a_first", "b_last", "indent_a", "indent_b", "a_stop",
    "a_comma", "b_upper", "b_lower", "b_digit", "a_list", "b_list", "a_cjk", "b_cjk", "len_a", "len_b", "a_label", "b_label", "comp_n", "comp_idx", "page_pitch_over_h",
    "rank_below", "rank_above", "h_over_page", "a_colon", "row_a", "row_b", "grid"};

QVector<LinePair> paragraph_line_pairs(const QVector<GroupingLine> &all) {
    // A detached marker takes no part; the line it belongs to counts as opening a list item.
    const auto owner = detached_list_markers(all);
    std::vector<bool> has_marker(static_cast<size_t>(all.size()));
    for (const int item : owner)
        if (item >= 0) has_marker[static_cast<size_t>(item)] = true;
    QVector<GroupingLine> lines;
    QVector<int> source;
    std::vector<bool> marked;
    for (int i = 0; i < all.size(); ++i) {
        if (owner[i] >= 0) continue;
        lines.append(all[i]);
        source.append(i);
        marked.push_back(has_marker[static_cast<size_t>(i)]);
    }
    const int count = static_cast<int>(lines.size());
    if (count == 0) return {};
    std::vector<Box> box(static_cast<size_t>(count));
    std::vector<double> height(box.size()), width(box.size()), middle(box.size());
    for (int i = 0; i < count; ++i) {
        const auto &rect = lines[i].rect;
        box[i] = {rect.left(), rect.top(), rect.right(), rect.bottom()};
        height[i] = std::max(1.0, box[i].y1 - box[i].y0);
        width[i] = box[i].x1 - box[i].x0;
        middle[i] = (box[i].y0 + box[i].y1) / 2;
    }
    const double typical_height = median_of(height);

    // Columns: lines loosely stacked under each other, with each column's own line pitch.
    std::vector<std::pair<int, int>> stacked;
    for (int a = 0; a < count; ++a)
        for (int b = 0; b < count; ++b)
            if (a != b && loosely_stacked(box[a], box[b])) stacked.emplace_back(a, b);
    const auto columns = components(count, stacked);
    std::vector<int> column_of(box.size()), rank_in_column(box.size());
    std::vector<double> column_left, column_width, column_pitch, column_size;
    for (int k = 0; k < columns.size(); ++k) {
        auto members = columns[k];
        std::stable_sort(members.begin(), members.end(), [&](int a, int b) { return middle[a] < middle[b]; });
        std::vector<double> pitches;
        for (int r = 0; r + 1 < members.size(); ++r) pitches.push_back(middle[members[r + 1]] - middle[members[r]]);
        column_pitch.push_back(pitches.empty() ? 1.2 * typical_height : std::max(percentile30(pitches), 0.05 * typical_height));
        double left = box[members.first()].x0, widest = width[members.first()];
        for (const int i : members) {
            left = std::min(left, box[i].x0);
            widest = std::max(widest, width[i]);
        }
        column_left.push_back(left);
        column_width.push_back(widest);
        column_size.push_back(static_cast<double>(members.size()));
        for (int r = 0; r < members.size(); ++r) {
            column_of[members[r]] = k;
            rank_in_column[members[r]] = r;
        }
    }

    // Candidates: a line below another that overlaps it and is within three line heights.
    std::vector<std::vector<int>> below(box.size()), above(box.size());
    for (int a = 0; a < count; ++a) {
        for (int b = 0; b < count; ++b) {
            if (a == b) continue;
            const double overlap = std::min(box[a].x1, box[b].x1) - std::max(box[a].x0, box[b].x0);
            if (overlap <= 0 || middle[b] <= middle[a] + 0.2 * height[a] || middle[b] - middle[a] > 3 * std::max(height[a], height[b])) continue;
            below[a].push_back(b);
        }
    }
    std::vector<double> nearest;
    for (int a = 0; a < count; ++a) {
        if (below[a].empty()) continue;
        double closest = middle[below[a].front()] - middle[a];
        for (const int b : below[a]) closest = std::min(closest, middle[b] - middle[a]);
        nearest.push_back(closest);
    }
    const double page_pitch = nearest.empty() ? 1.2 * typical_height : median_of(nearest);
    std::vector<std::vector<int>> rank_below(box.size());
    for (int a = 0; a < count; ++a) {
        auto order = below[a];
        std::stable_sort(order.begin(), order.end(), [&](int x, int y) { return middle[x] - middle[a] < middle[y] - middle[a]; });
        rank_below[a].assign(below[a].size(), 0);
        for (size_t r = 0; r < order.size(); ++r)
            rank_below[a][static_cast<size_t>(std::find(below[a].begin(), below[a].end(), order[r]) - below[a].begin())] = static_cast<int>(r);
        for (const int b : below[a]) above[b].push_back(a);
    }
    std::vector<std::vector<std::pair<int, int>>> rank_above(box.size());  // (upper line, rank) for each lower line
    for (int b = 0; b < count; ++b) {
        auto order = above[b];
        std::stable_sort(order.begin(), order.end(), [&](int x, int y) { return middle[b] - middle[x] < middle[b] - middle[y]; });
        for (size_t r = 0; r < order.size(); ++r) rank_above[b].emplace_back(order[r], static_cast<int>(r));
    }

    struct Text {
        bool list = false, label = false, stop = false, comma = false, colon = false, upper = false, lower = false, digit = false;
        double cjk = 0, length = 0;
    };
    std::vector<Text> text(box.size());
    for (int i = 0; i < count; ++i) {
        const auto &t = lines[i].text;
        auto &info = text[i];
        info.list = marked[static_cast<size_t>(i)] || starts_list_item(t);
        info.label = looks_like_short_label(t, box[i]);
        info.stop = ends_in(t, STOPS);
        info.comma = ends_in(t, COMMAS);
        info.colon = ends_in(t, COLONS);
        info.upper = !t.isEmpty() && t.front().isUpper();
        info.lower = !t.isEmpty() && t.front().isLower();
        info.digit = !t.isEmpty() && t.front().isDigit();
        info.cjk = cjk_share(t);
        info.length = static_cast<double>(t.size());
    }

    // Lines beside a line on the same row (half of the lower one's height shared, no horizontal
    // overlap): the other cells of a table row. Two lines whose rows line up cell for cell sit in a grid.
    std::vector<std::vector<int>> beside(box.size());
    for (int i = 0; i < count; ++i) {
        for (int j = 0; j < count; ++j) {
            if (i == j) continue;
            const double shared = std::min(box[i].y1, box[j].y1) - std::max(box[i].y0, box[j].y0);
            if (shared > 0.5 * std::min(height[i], height[j]) && !(box[j].x1 > box[i].x0 && box[j].x0 < box[i].x1)) beside[i].push_back(j);
        }
    }

    QVector<LinePair> pairs;
    for (int a = 0; a < count; ++a) {
        for (size_t k = 0; k < below[a].size(); ++k) {
            const int b = below[a][k];
            const double ha = height[a];
            int grid = 0;
            for (const int i : beside[a])
                for (const int j : beside[b])
                    if (std::abs(box[j].x0 - box[i].x0) <= 0.5 * ha) {
                        ++grid;
                        break;
                    }
            const int ka = column_of[a], kb = column_of[b];
            const double overlap = std::min(box[a].x1, box[b].x1) - std::max(box[a].x0, box[b].x0);
            const double centre_a = (box[a].x0 + box[a].x1) / 2, centre_b = (box[b].x0 + box[b].x1) / 2;
            const double pitch = middle[b] - middle[a];
            int above_rank = 0;
            for (const auto &[upper, rank] : rank_above[b])
                if (upper == a) above_rank = rank;
            const double values[PARAGRAPH_FEATURE_COUNT] = {
                (box[b].y0 - box[a].y1) / ha, pitch / ha, pitch / page_pitch, pitch / (ka == kb ? column_pitch[ka] : page_pitch),
                (box[b].x0 - box[a].x0) / ha, (box[b].x1 - box[a].x1) / ha, (centre_b - centre_a) / ha, width[a] / ha, width[b] / ha, width[b] / std::max(1.0, width[a]),
                overlap / std::max(1.0, std::min(width[a], width[b])), height[b] / ha, width[a] / std::max(1.0, column_width[ka]), width[b] / std::max(1.0, column_width[kb]),
                rank_in_column[a] == 0 ? 1.0 : 0.0, rank_in_column[b] == column_size[kb] - 1 ? 1.0 : 0.0, (box[a].x0 - column_left[ka]) / ha, (box[b].x0 - column_left[kb]) / ha,
                text[a].stop ? 1.0 : 0.0, text[a].comma ? 1.0 : 0.0, text[b].upper ? 1.0 : 0.0, text[b].lower ? 1.0 : 0.0, text[b].digit ? 1.0 : 0.0,
                text[a].list ? 1.0 : 0.0, text[b].list ? 1.0 : 0.0, text[a].cjk, text[b].cjk, text[a].length, text[b].length,
                text[a].label ? 1.0 : 0.0, text[b].label ? 1.0 : 0.0, column_size[ka], static_cast<double>(rank_in_column[a]),
                page_pitch / typical_height, static_cast<double>(rank_below[a][k]), static_cast<double>(above_rank), ha / typical_height, text[a].colon ? 1.0 : 0.0,
                static_cast<double>(beside[a].size()), static_cast<double>(beside[b].size()), static_cast<double>(grid)};
            LinePair pair;
            pair.above = source[a];
            pair.below = source[b];
            for (int f = 0; f < PARAGRAPH_FEATURE_COUNT; ++f) pair.features[f] = static_cast<float>(values[f]);
            pairs.append(pair);
        }
    }
    return pairs;
}

QVector<QVector<int>> group_lines_into_paragraphs(const QVector<GroupingLine> &lines) {
    const int count = static_cast<int>(lines.size());
    if (count == 0) return {};
    // Each line keeps its best partner below and above; a pair both lines choose is a link.
    std::vector<int> best_below(static_cast<size_t>(count), -1), best_above(static_cast<size_t>(count), -1);
    std::vector<double> best_below_score(static_cast<size_t>(count)), best_above_score(static_cast<size_t>(count));
    for (const auto &pair : paragraph_line_pairs(lines)) {
        const double s = score(pair.features);
        if (s < 0) continue;
        if (best_below[pair.above] < 0 || s > best_below_score[pair.above]) {
            best_below[pair.above] = pair.below;
            best_below_score[pair.above] = s;
        }
        if (best_above[pair.below] < 0 || s > best_above_score[pair.below]) {
            best_above[pair.below] = pair.above;
            best_above_score[pair.below] = s;
        }
    }
    std::vector<std::pair<int, int>> links;
    for (int a = 0; a < count; ++a)
        if (best_below[a] >= 0 && best_above[best_below[a]] == a) links.emplace_back(a, best_below[a]);

    std::vector<Box> box(static_cast<size_t>(count));
    std::vector<bool> label(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        box[i] = {lines[i].rect.left(), lines[i].rect.top(), lines[i].rect.right(), lines[i].rect.bottom()};
        label[i] = is_short_label(lines[i]);
    }
    // A detached marker goes just before the line it belongs to, left to right if there are several.
    const auto owner = detached_list_markers(lines);
    std::vector<QVector<int>> markers(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
        if (owner[i] >= 0) markers[static_cast<size_t>(owner[i])].append(i);
    for (auto &list : markers) std::stable_sort(list.begin(), list.end(), [&](int a, int b) { return box[a].x0 < box[b].x0; });
    QVector<QVector<int>> paragraphs;
    const auto add = [&paragraphs, &markers](const QVector<int> &piece) {
        for (int start = 0; start < piece.size(); start += MAX_PARAGRAPH_LINES) {
            QVector<int> paragraph;
            for (const int line : piece.mid(start, MAX_PARAGRAPH_LINES)) paragraph << markers[static_cast<size_t>(line)] << line;
            paragraphs.append(paragraph);
        }
    };
    for (auto group : components(count, links)) {
        if (owner[group.first()] >= 0) continue;  // a marker has no links: it is placed with its line
        std::stable_sort(group.begin(), group.end(), [&](int a, int b) {
            if (box[a].y0 != box[b].y0) return box[a].y0 < box[b].y0;
            return box[a].x0 < box[b].x0;
        });
        // A list item starts a paragraph, and two menu entries stacked as tightly as prose stay apart.
        QVector<int> piece{group.first()};
        for (int position = 1; position < group.size(); ++position) {
            const int line = group[position];
            if (!markers[static_cast<size_t>(line)].isEmpty() || opens_list_item(lines[line].text) || (piece.size() == 1 && label[piece.first()] && label[line])) {
                add(piece);
                piece = {line};
            } else {
                piece.append(line);
            }
        }
        add(piece);
    }
    return paragraphs;
}
}
