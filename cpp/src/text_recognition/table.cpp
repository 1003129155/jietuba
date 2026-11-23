#include "text_recognition/table.h"
#include "ocr/ocr_line_geometry.h"
#include <QPolygonF>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <tuple>
#include <vector>

namespace jietuba::text_table {
namespace {
// Characters written without spaces between them: kana, CJK ideographs, full-width forms.
bool is_cjk(QChar c) {
    const auto u = c.unicode();
    return (u >= 0x3040 && u <= 0x30FF) || (u >= 0x3400 && u <= 0x9FFF) || (u >= 0xF900 && u <= 0xFAFF) || (u >= 0xFF00 && u <= 0xFFEF);
}
bool ends_cjk(const QString &s) { return !s.isEmpty() && is_cjk(s.back()); }
bool starts_cjk(const QString &s) { return !s.isEmpty() && is_cjk(s.front()); }
bool has_cjk(const QString &s) { return std::any_of(s.begin(), s.end(), [](QChar c) { return is_cjk(c); }); }

// Dots, bullets and boxes the engine reads from icons and list markers.
bool is_junk(const QString &text) {
    if (text.isEmpty()) return false;
    return std::all_of(text.begin(), text.end(), [](QChar c) {
        const auto u = c.unicode();
        return u == '.' || u == ',' || u == '\'' || u == '`' || u == 0x00B7 || u == 0x2022 || u == 0x30FB || u == 0xFF0C || u == 0x3001 ||
               (u >= 0x25A0 && u <= 0x25FF) || u == 0x2B1B || u == 0x2B1C;
    });
}

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const auto n = values.size();
    if (n == 0) return 0;
    return n % 2 ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) / 2;
}

int trunc_int(double v) { return static_cast<int>(std::trunc(v)); }

// Trees trained on screenshots whose cells are known (scripts/table_model): one scores whether a
// visual row continues the table row above it, the other whether a visual row belongs to the table
// at all; a positive score says yes. A child index at or above zero is another split, below zero
// the leaf ~index.
struct ModelSplit {
    float threshold;
    short left;
    short right;
    unsigned char feature;
};
namespace row_model {
#include "text_recognition/table_row_model.inc"
}
namespace extent_model {
#include "text_recognition/table_extent_model.inc"
}
namespace caption_model {
#include "text_recognition/table_caption_model.inc"
}
static_assert(caption_model::MODEL_FEATURE_COUNT == CAPTION_FEATURE_COUNT, "the caption model was trained on another set of features");
static_assert(row_model::MODEL_FEATURE_COUNT == ROW_FEATURE_COUNT, "the row model was trained on another set of features");
static_assert(extent_model::MODEL_FEATURE_COUNT == EXTENT_FEATURE_COUNT, "the extent model was trained on another set of features");
// The trees overrule the rules only with this much confidence; a score closer to zero keeps the
// rules' answer. Kept equal to MARGIN in scripts/table_model/train.py.
constexpr double ROW_OVERRIDE_MARGIN = 0.5;
constexpr double EXTENT_OVERRIDE_MARGIN = 0.5;
// The rules keep no title or note; the trees keep one with the same confidence.
constexpr double CAPTION_KEEP_SCORE = 0.5;

template <size_t N, size_t S, size_t L, size_t T>
double tree_score(const std::array<float, N> &features, double baseline, const ModelSplit (&splits)[S], const double (&leaves)[L], const short (&trees)[T]) {
    double raw = baseline;
    for (const short root : trees) {
        int node = root;
        while (node >= 0) {
            const auto &split = splits[node];
            node = features[split.feature] <= split.threshold ? split.left : split.right;
        }
        raw += leaves[~node];
    }
    return raw;
}
double row_score(const std::array<float, ROW_FEATURE_COUNT> &f) {
    return tree_score(f, row_model::MODEL_BASELINE, row_model::MODEL_SPLITS, row_model::MODEL_LEAVES, row_model::MODEL_TREES);
}
double extent_score(const std::array<float, EXTENT_FEATURE_COUNT> &f) {
    return tree_score(f, extent_model::MODEL_BASELINE, extent_model::MODEL_SPLITS, extent_model::MODEL_LEAVES, extent_model::MODEL_TREES);
}
double caption_score(const std::array<float, CAPTION_FEATURE_COUNT> &f) {
    return tree_score(f, caption_model::MODEL_BASELINE, caption_model::MODEL_SPLITS, caption_model::MODEL_LEAVES, caption_model::MODEL_TREES);
}

struct PlacedWord {
    QString text;
    double x0, x1, y0, y1;
    // Place in the line's text, -1 when the word is not found in it.
    int start, end;
};

// Words of one line that belong together: a cell, or several cells the engine read as one line.
struct Seg {
    std::vector<PlacedWord> words;
    QString line_text;
    double x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    // The segment as read, before splitting at column boundaries.
    int origin = -1;

    Seg(std::vector<PlacedWord> w, QString text) : words(std::move(w)), line_text(std::move(text)) {
        x0 = y0 = 1e18;
        x1 = y1 = -1e18;
        for (const auto &word : words) {
            x0 = std::min(x0, word.x0);
            x1 = std::max(x1, word.x1);
            y0 = std::min(y0, word.y0);
            y1 = std::max(y1, word.y1);
        }
    }
    double h() const { return y1 - y0; }
    double yc() const { return (y0 + y1) / 2; }
    QString text() const {
        if (std::all_of(words.begin(), words.end(), [](const PlacedWord &w) { return w.start >= 0; }))
            return line_text.mid(words.front().start, std::max(0, words.back().end - words.front().start)).trimmed();
        QString out;
        for (const auto &w : words) {
            if (!out.isEmpty() && !(ends_cjk(out) || starts_cjk(w.text))) out += ' ';
            out += w.text;
        }
        return out;
    }
};

struct Rule {
    double pos;
    int start, end;
};

using Row = std::vector<int>;
using ColumnSet = std::set<int>;

// (start, end) of runs of mask bridged over bridge pixels, at least min_len long and half real.
void runs(const std::vector<uint8_t> &mask, const std::vector<uint8_t> &bridge, double min_len, double pos, std::vector<Rule> &out) {
    const int n = static_cast<int>(mask.size());
    int i = 0;
    while (i < n) {
        if (!(mask[i] || bridge[i])) {
            ++i;
            continue;
        }
        const int s = i;
        while (i < n && (mask[i] || bridge[i])) ++i;
        int first = -1, last = -1, real = 0;
        for (int k = s; k < i; ++k)
            if (mask[k]) {
                if (first < 0) first = k;
                last = k;
                ++real;
            }
        if (first < 0) continue;
        const int e = last + 1;
        if (e - first >= min_len && real >= 0.5 * (e - first)) out.push_back({pos, first, e});
    }
}

// Runs at nearby positions that overlap along their length become one rule.
std::vector<Rule> merge_rules(std::vector<Rule> rules) {
    std::sort(rules.begin(), rules.end(), [](const Rule &a, const Rule &b) {
        return std::tie(a.pos, a.start, a.end) < std::tie(b.pos, b.start, b.end);
    });
    struct Merged {
        double pos;
        int start, end;
        double last;
        int count;
    };
    std::vector<Merged> out;
    for (const auto &r : rules) {
        bool merged = false;
        for (auto o = out.rbegin(); o != out.rend(); ++o) {
            if (r.pos - o->last > 3.5) break;
            if (r.start <= o->end + 2 && r.end >= o->start - 2) {
                o->pos = (o->pos * o->count + r.pos) / (o->count + 1);
                o->start = std::min(o->start, r.start);
                o->end = std::max(o->end, r.end);
                o->last = r.pos;
                ++o->count;
                merged = true;
                break;
            }
        }
        if (!merged) out.push_back({r.pos, r.start, r.end, r.pos, 1});
    }
    std::vector<Rule> result;
    for (const auto &o : out) result.push_back({o.pos, o.start, o.end});
    return result;
}

// Luma as PIL computes it, which the thresholds were tuned with.
std::vector<uint8_t> gray_of(const QImage &image) {
    const QImage rgb = image.convertToFormat(QImage::Format_RGB32);
    std::vector<uint8_t> gray(static_cast<size_t>(rgb.width()) * rgb.height());
    for (int y = 0; y < rgb.height(); ++y) {
        const auto *line = reinterpret_cast<const QRgb *>(rgb.constScanLine(y));
        for (int x = 0; x < rgb.width(); ++x)
            gray[static_cast<size_t>(y) * rgb.width() + x] =
                static_cast<uint8_t>((qRed(line[x]) * 19595 + qGreen(line[x]) * 38470 + qBlue(line[x]) * 7471 + 0x8000) >> 16);
    }
    return gray;
}

class Builder {
public:
    Builder(const QImage &image, const QVector<Line> &lines, const RowControl &control) : image_(image), control_(control) { make_segments(lines); }
    Table build();

private:
    // ---- Segments and rows ----
    void make_segments(const QVector<Line> &lines);
    std::vector<Row> visual_rows() const;
    double top(const Row &r) const {
        double v = 1e18;
        for (int s : r) v = std::min(v, segs_[s].y0);
        return v;
    }
    double bottom(const Row &r) const {
        double v = -1e18;
        for (int s : r) v = std::max(v, segs_[s].y1);
        return v;
    }
    double gap(const Row &a, const Row &b) const { return top(b) - bottom(a); }
    int split(int seg, double x, double reach);

    // ---- Rules ----
    void detect_rules();
    bool hrule_between(double ya, double yb, double x0, double x1, double min_len) const {
        for (const auto &r : hrules_)
            if (ya - 2 <= r.pos && r.pos <= yb + 2 && std::min<double>(r.end, x1) - std::max<double>(r.start, x0) >= min_len) return true;
        return false;
    }
    bool vrule_at(double x, double y0, double y1) const {
        for (const auto &r : vrules_)
            if (std::abs(r.pos - x) <= 3 && r.start <= y0 + 1 && r.end >= y1 - 1) return true;
        return false;
    }

    // ---- Columns ----
    int spanning(int seg, const std::vector<Row> &rows, size_t own) const;
    bool multi_cell(const Row &row) const;
    void find_columns(const std::vector<Row> &rows, bool merged);
    int col_of(double x) const {
        int i = 0;
        while (i < static_cast<int>(bounds_.size()) && x > bounds_[i]) ++i;
        return i;
    }
    std::pair<int, int> seg_cols(int seg) const {
        const auto &s = segs_[seg];
        const double inset = 0.25 * std::min(s.h(), s.x1 - s.x0);
        return {col_of(s.x0 + inset), col_of(s.x1 - inset)};
    }
    ColumnSet row_cols(const Row &row) const {
        ColumnSet out;
        for (int s : row) {
            const auto [c0, c1] = seg_cols(s);
            for (int c = c0; c <= c1; ++c) out.insert(c);
        }
        return out;
    }

    // ---- Context ----
    std::vector<Row> drop_context(std::vector<Row> rows) const;
    double fill_of(const Row &row, double x0, double x1) const;
    std::vector<Row> table_extent(const std::vector<Row> &all, const std::vector<Row> &kept, const std::function<int(const Row &)> &straddles);
    QString join_text(std::vector<int> parts) const;

    QImage image_;
    const RowControl &control_;
    std::vector<uint8_t> gray_;
    std::vector<Seg> segs_;
    double text_h_ = 0;
    std::vector<Rule> hrules_, vrules_;
    std::vector<double> bounds_;
    // Rows just outside the table kept as its title (above) and its note (below), top to bottom.
    std::vector<Row> title_, note_;
    bool lattice_ = false;
};

void Builder::make_segments(const QVector<Line> &lines) {
    struct Group {
        std::vector<PlacedWord> words;
        QString line_text;
        double y0, y1;
    };
    std::vector<Group> all;
    for (const auto &line : lines) {
        std::vector<PlacedWord> words;
        int pos = 0;
        auto add = [&](const QString &text, double x0, double x1, double y0, double y1) {
            const int i = static_cast<int>(line.text.indexOf(text, pos));
            if (i >= 0) {
                words.push_back({text, x0, x1, y0, y1, i, i + static_cast<int>(text.size())});
                pos = i + static_cast<int>(text.size());
            } else {
                words.push_back({text, x0, x1, y0, y1, -1, -1});
            }
        };
        if (line.words.isEmpty())
            add(line.text, line.x0, line.x1, line.y0, line.y1);
        else
            for (const auto &w : line.words) add(w.text, w.x0, w.x1, w.y0, w.y1);
        std::stable_sort(words.begin(), words.end(), [](const PlacedWord &a, const PlacedWord &b) { return a.x0 < b.x0; });
        // Words further apart than the line is high are separate cells even on one line.
        const double h = line.y1 - line.y0;
        std::vector<std::vector<PlacedWord>> groups{{words.front()}};
        for (size_t k = 1; k < words.size(); ++k) {
            if (words[k].x0 - groups.back().back().x1 > 1.0 * h) groups.emplace_back();
            groups.back().push_back(words[k]);
        }
        for (auto &g : groups) {
            double y0 = 1e18, y1 = -1e18;
            for (const auto &w : g) {
                y0 = std::min(y0, w.y0);
                y1 = std::max(y1, w.y1);
            }
            all.push_back({std::move(g), line.text, y0, y1});
        }
    }
    // Lines cut short by the top or bottom edge of the selection are text around the table; a
    // tight crop through a row of the table leaves some of its lines at full height.
    std::vector<double> heights;
    for (const auto &g : all) heights.push_back(g.y1 - g.y0);
    const double line_h = median(heights);
    const int height = image_.height();
    std::vector<uint8_t> cut(all.size(), 0);
    for (const bool at_top : {true, false}) {
        double tallest = -1;
        for (const auto &g : all)
            if (at_top ? g.y0 <= 1 : g.y1 >= height - 2) tallest = std::max(tallest, g.y1 - g.y0);
        if (tallest < 0 || tallest >= 0.8 * line_h) continue;
        for (size_t k = 0; k < all.size(); ++k)
            if (at_top ? all[k].y0 <= 1 : all[k].y1 >= height - 2) cut[k] = 1;
    }
    for (size_t k = 0; k < all.size(); ++k) {
        if (cut[k]) continue;
        std::vector<PlacedWord> kept;
        for (auto &w : all[k].words)
            if (!is_junk(w.text)) kept.push_back(w);
        if (kept.empty()) continue;
        segs_.emplace_back(std::move(kept), all[k].line_text);
        segs_.back().origin = static_cast<int>(segs_.size()) - 1;
    }
}

std::vector<Row> Builder::visual_rows() const {
    std::vector<int> order(segs_.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [this](int a, int b) { return segs_[a].yc() < segs_[b].yc(); });
    std::vector<Row> rows;
    for (int s : order) {
        const auto &seg = segs_[s];
        Row *best = nullptr;
        for (size_t k = rows.size() > 4 ? rows.size() - 4 : 0; k < rows.size(); ++k) {
            std::vector<double> y0s, y1s;
            for (int m : rows[k]) {
                y0s.push_back(segs_[m].y0);
                y1s.push_back(segs_[m].y1);
            }
            const double y0 = median(y0s), y1 = median(y1s);
            if (std::min(y1, seg.y1) - std::max(y0, seg.y0) >= 0.5 * std::min(y1 - y0, seg.h())) best = &rows[k];
        }
        if (best)
            best->push_back(s);
        else
            rows.push_back({s});
    }
    for (auto &r : rows) std::stable_sort(r.begin(), r.end(), [this](int a, int b) { return segs_[a].x0 < segs_[b].x0; });
    return rows;
}

int Builder::split(int seg, double x, double reach) {
    const auto &s = segs_[seg];
    std::optional<std::pair<double, size_t>> best;
    for (size_t i = 0; i + 1 < s.words.size(); ++i) {
        const auto &a = s.words[i], &b = s.words[i + 1];
        // Words the engine joined without a space belong to one cell.
        if (a.end >= 0 && b.start >= 0 && !s.line_text.mid(a.end, std::max(0, b.start - a.end)).trimmed().isEmpty()) continue;
        if (a.end >= 0 && a.end == b.start) continue;
        const double lo = a.x1, hi = b.x0;
        const double d = lo <= x && x <= hi + 1 ? 0 : std::min(std::abs(x - lo), std::abs(x - hi));
        // Only between Latin or digit words: CJK text over a boundary is one cell spanning it.
        const bool cjk = ends_cjk(a.text) || starts_cjk(b.text);
        if (d <= (cjk ? 0 : reach) && (!best || d < best->first)) best = std::make_pair(d, i);
    }
    if (!best) return -1;
    const size_t i = best->second;
    const int origin = s.origin;
    const QString text = s.line_text;
    std::vector<PlacedWord> left(s.words.begin(), s.words.begin() + static_cast<long>(i) + 1);
    std::vector<PlacedWord> right(s.words.begin() + static_cast<long>(i) + 1, s.words.end());
    segs_.emplace_back(std::move(left), text);
    segs_.back().origin = origin;
    segs_.emplace_back(std::move(right), text);
    segs_.back().origin = origin;
    return static_cast<int>(segs_.size()) - 2;
}

void Builder::detect_rules() {
    const int width = image_.width(), height = image_.height();
    if (width < 2 || height < 2) return;
    gray_ = gray_of(image_);
    const auto &gray = gray_;
    std::vector<uint8_t> text(static_cast<size_t>(width) * height, 0);
    for (const auto &s : segs_)
        for (const auto &w : s.words) {
            const int ya = std::max(0, trunc_int(w.y0) - 1), yb = std::min(height, trunc_int(w.y1) + 2);
            const int xa = std::max(0, trunc_int(w.x0) - 1), xb = std::min(width, trunc_int(w.x1) + 2);
            for (int y = ya; y < yb; ++y) std::fill(text.begin() + static_cast<long>(y) * width + xa, text.begin() + static_cast<long>(y) * width + std::max(xa, xb), 1);
        }
    // Edges between pixels that differ, outside text; text pixels bridge a line passing behind words.
    constexpr int THRESHOLD = 8;
    const double hmin = std::max(2 * text_h_, 24.0), vmin = std::max(0.8 * text_h_, 10.0);
    std::vector<Rule> hr, vr;
    std::vector<uint8_t> mask(width), bridge(width);
    for (int y = 0; y + 1 < height; ++y) {
        const size_t a = static_cast<size_t>(y) * width, b = a + width;
        for (int x = 0; x < width; ++x) {
            bridge[x] = text[a + x] || text[b + x];
            mask[x] = !bridge[x] && std::abs(gray[b + x] - gray[a + x]) > THRESHOLD;
        }
        runs(mask, bridge, hmin, y + 0.5, hr);
    }
    mask.assign(height, 0);
    bridge.assign(height, 0);
    for (int x = 0; x + 1 < width; ++x) {
        for (int y = 0; y < height; ++y) {
            const size_t a = static_cast<size_t>(y) * width + x;
            bridge[y] = text[a] || text[a + 1];
            mask[y] = !bridge[y] && std::abs(gray[a + 1] - gray[a]) > THRESHOLD;
        }
        runs(mask, bridge, vmin, x + 0.5, vr);
    }
    hrules_ = merge_rules(std::move(hr));
    vrules_ = merge_rules(std::move(vr));
}

int Builder::spanning(int seg, const std::vector<Row> &rows, size_t own) const {
    const auto &s = segs_[seg];
    int n = 0;
    for (size_t k = 0; k < rows.size(); ++k) {
        if (k == own) continue;
        int hit = 0;
        for (int t : rows[k])
            if (segs_[t].x0 < s.x1 && segs_[t].x1 > s.x0) ++hit;
        if (hit >= 2) ++n;
    }
    return n;
}

bool Builder::multi_cell(const Row &row) const {
    if (row.size() >= 2) return true;
    auto words = segs_[row.front()].words;
    std::stable_sort(words.begin(), words.end(), [](const PlacedWord &a, const PlacedWord &b) { return a.x0 < b.x0; });
    for (size_t k = 0; k + 1 < words.size(); ++k)
        if (words[k + 1].x0 - words[k].x1 >= 0.5 * text_h_) return true;
    return false;
}

void Builder::find_columns(const std::vector<Row> &rows, bool merged) {
    double tx0 = 1e18, tx1 = -1e18, ty0 = 1e18, ty1 = -1e18;
    for (const auto &r : rows)
        for (int s : r) {
            tx0 = std::min(tx0, segs_[s].x0);
            tx1 = std::max(tx1, segs_[s].x1);
            ty0 = std::min(ty0, segs_[s].y0);
            ty1 = std::max(ty1, segs_[s].y1);
        }
    const double nrows = static_cast<double>(rows.size());
    // Components: x ranges where segments that span no two cells of another row overlap.
    std::vector<int> narrow;
    for (size_t k = 0; k < rows.size(); ++k)
        for (int s : rows[k]) {
            const int n = spanning(s, rows, k);
            if (n == 0 || (n < 2 && n < 0.15 * nrows)) narrow.push_back(s);
        }
    std::stable_sort(narrow.begin(), narrow.end(), [this](int a, int b) { return segs_[a].x0 < segs_[b].x0; });
    std::vector<std::pair<double, double>> comps;
    for (int s : narrow) {
        if (!comps.empty() && segs_[s].x0 < comps.back().second)
            comps.back().second = std::max(comps.back().second, segs_[s].x1);
        else
            comps.push_back({segs_[s].x0, segs_[s].x1});
    }

    // Whitespace rivers: x ranges no word covers in any row of several cells. They also part
    // cells the engine read as one line.
    const int x0i = trunc_int(tx0);
    const int width = static_cast<int>(std::ceil(tx1)) - x0i + 2;
    std::vector<int> cov(std::max(0, width), 0);
    std::vector<const Row *> counted;
    for (const auto &r : rows) {
        if (!(merged ? multi_cell(r) : r.size() >= 2)) continue;
        counted.push_back(&r);
        std::vector<uint8_t> m(cov.size(), 0);
        for (int s : r)
            for (const auto &w : segs_[s].words) {
                const int a = std::max(0, trunc_int(w.x0) - x0i), b = std::min(width, static_cast<int>(std::ceil(w.x1)) - x0i);
                for (int x = a; x < b; ++x) m[x] = 1;
            }
        for (int x = 0; x < width; ++x) cov[x] += m[x];
    }
    // Narrow gaps between CJK characters line up in columns of equal-length names; a narrow
    // river counts only when most rows have a Latin or digit word beside it.
    auto latin_flanked = [&](int a, int b) {
        int both = 0, latin = 0;
        for (const Row *r : counted) {
            const PlacedWord *lw = nullptr, *rw = nullptr;
            for (int s : *r)
                for (const auto &w : segs_[s].words) {
                    if (w.x1 - x0i <= a + 1 && w.x1 - x0i > a - 1.5 * text_h_ && (!lw || w.x1 > lw->x1)) lw = &w;
                    if (w.x0 - x0i >= b - 1 && w.x0 - x0i < b + 1.5 * text_h_ && (!rw || w.x0 < rw->x0)) rw = &w;
                }
            if (lw && rw) {
                ++both;
                if (!ends_cjk(lw->text) || !starts_cjk(rw->text)) ++latin;
            }
        }
        return both > 0 && latin >= 0.8 * both;
    };
    // Spaces between words of one cell, so that a narrow river is not a space repeated in every
    // row; a low percentile, as merged lines add the wider gaps between cells.
    std::vector<double> spaces;
    for (const auto &r : rows)
        for (int s : r) {
            const auto &w = segs_[s].words;
            for (size_t k = 0; k + 1 < w.size(); ++k) {
                const double g = w[k + 1].x0 - w[k].x1;
                if (g > 0 && g < 0.6 * text_h_ && !ends_cjk(w[k].text) && !starts_cjk(w[k + 1].text)) spaces.push_back(g);
            }
        }
    std::sort(spaces.begin(), spaces.end());
    const double space = spaces.empty() ? 0.25 * text_h_ : spaces[spaces.size() / 10];
    std::vector<std::pair<int, int>> rivers;
    for (int x = 0; x < width;) {
        if (cov[x]) {
            ++x;
            continue;
        }
        const int a = x;
        while (x < width && !cov[x]) ++x;
        const int b = x;
        if (a > 1 && b < width - 2 && (b - a >= 0.4 * text_h_ || (b - a >= std::max(0.25 * text_h_, 1.2 * space) && latin_flanked(a, b))))
            rivers.push_back({a, b});
    }
    // A river must open a column that more than one row fills; otherwise it is a gap inside a cell.
    while (!rivers.empty() && rows.size() >= 3) {
        int weak = -1;
        for (size_t i = 0; i <= rivers.size() && weak < 0; ++i) {
            const int lo = i == 0 ? 0 : rivers[i - 1].second;
            const int hi = i < rivers.size() ? rivers[i].first : width;
            if (hi <= lo || *std::max_element(cov.begin() + lo, cov.begin() + hi) < 2) weak = static_cast<int>(i);
        }
        if (weak < 0) break;
        int drop = -1;
        for (int k : {weak - 1, weak})
            if (k >= 0 && k < static_cast<int>(rivers.size()) &&
                (drop < 0 || rivers[k].second - rivers[k].first < rivers[drop].second - rivers[drop].first))
                drop = k;
        rivers.erase(rivers.begin() + drop);
    }
    std::vector<std::pair<double, double>> global;
    for (const auto &[a, b] : rivers) global.push_back({static_cast<double>(x0i + a), static_cast<double>(x0i + b)});

    // A gap between components holding text of merged lines yields to the rivers inside it.
    std::vector<std::pair<double, double>> gaps;
    for (size_t k = 0; k + 1 < comps.size(); ++k) {
        const double g0 = comps[k].second, g1 = comps[k + 1].first;
        bool any = false;
        for (const auto &[r0, r1] : global)
            if (r0 <= g1 + 0.5 * text_h_ && r1 >= g0 - 0.5 * text_h_) {
                gaps.push_back({std::max(r0, g0), std::min(r1, g1)});
                any = true;
            }
        if (!any) gaps.push_back({g0, g1});
    }
    // Rivers inside a component count unless lines of several one-segment rows (wrapped cells)
    // cross them; one such row is a header over two columns.
    std::vector<const Row *> singles;
    for (const auto &r : rows)
        if (!multi_cell(r)) singles.push_back(&r);
    for (const auto &[r0, r1] : global) {
        bool overlaps = false;
        for (const auto &[g0, g1] : gaps)
            if (r0 <= g1 && r1 >= g0) overlaps = true;
        if (overlaps) continue;
        int crossing = 0;
        for (const Row *r : singles)
            for (const auto &w : segs_[r->front()].words)
                if (w.x0 < r1 - 2 && w.x1 > r0 + 2) {
                    ++crossing;
                    break;
                }
        if (crossing <= 1) gaps.push_back({r0, r1});
    }
    std::sort(gaps.begin(), gaps.end());

    // Boundaries snap to vertical rules in the gaps; rules in most gaps make a lattice.
    std::vector<Rule> vx;
    for (const auto &v : vrules_)
        if (tx0 + 2 < v.pos && v.pos < tx1 - 2 && v.end - v.start >= 1.2 * text_h_) vx.push_back(v);
    bounds_.clear();
    int covered = 0;
    for (const auto &[lo, hi] : gaps) {
        const Rule *best = nullptr;
        for (const auto &v : vx)
            if (lo - 2 <= v.pos && v.pos <= hi + 2 && (!best || v.end - v.start > best->end - best->start)) best = &v;
        if (best) {
            ++covered;
            bounds_.push_back(best->pos);
        } else {
            bounds_.push_back((lo + hi) / 2);
        }
    }
    lattice_ = !gaps.empty() && covered >= 0.6 * static_cast<double>(gaps.size());
    if (lattice_) {
        for (const auto &v : vx)
            if (std::all_of(bounds_.begin(), bounds_.end(), [&](double b) { return std::abs(v.pos - b) > 3; }) && v.end - v.start >= 0.3 * (ty1 - ty0))
                bounds_.push_back(v.pos);
        std::sort(bounds_.begin(), bounds_.end());
    }
}

std::vector<Row> Builder::drop_context(std::vector<Row> rows) const {
    if (rows.size() < 3) return rows;
    double tx0 = 1e18, tx1 = -1e18;
    for (const auto &r : rows)
        for (int s : r) {
            tx0 = std::min(tx0, segs_[s].x0);
            tx1 = std::max(tx1, segs_[s].x1);
        }
    std::optional<double> frame_top, frame_bottom;
    for (const auto &h : hrules_)
        if (h.end - h.start >= 0.5 * (tx1 - tx0)) {
            frame_top = std::min(frame_top.value_or(h.pos), h.pos);
            frame_bottom = std::max(frame_bottom.value_or(h.pos), h.pos);
        }
    std::vector<size_t> counts;
    for (const auto &r : rows) counts.push_back(r.size());
    std::sort(counts.begin(), counts.end());
    const size_t typical = counts[counts.size() / 2];
    std::vector<double> gaps;
    for (size_t k = 0; k + 1 < rows.size(); ++k) gaps.push_back(gap(rows[k], rows[k + 1]));
    std::sort(gaps.begin(), gaps.end());
    const double med_gap = gaps[gaps.size() / 2];

    // Widths of word runs with no gap wider than 0.6 line heights.
    auto runs_of = [&](const Row &row) {
        std::vector<PlacedWord> words;
        for (int s : row) words.insert(words.end(), segs_[s].words.begin(), segs_[s].words.end());
        std::stable_sort(words.begin(), words.end(), [](const PlacedWord &a, const PlacedWord &b) { return a.x0 < b.x0; });
        std::vector<double> out;
        double start = words[0].x0, end = words[0].x1;
        for (size_t k = 1; k < words.size(); ++k) {
            if (words[k].x0 - end > 0.6 * text_h_) {
                out.push_back(end - start);
                start = words[k].x0;
            }
            end = std::max(end, words[k].x1);
        }
        out.push_back(end - start);
        return out;
    };
    auto paragraph = [&](const Row &row) {
        if (row.size() != 1) return false;
        const auto widths = runs_of(row);
        return *std::max_element(widths.begin(), widths.end()) > 0.6 * (tx1 - tx0);
    };
    // Rows inside the frame of long rules, or pressed against the next row (a header over a
    // row span), stay.
    auto context = [&](const Row &row, double g, const Row &neighbour, bool leading) {
        if (row.size() != 1) return false;
        const auto &s = segs_[row.front()];
        if (frame_top && *frame_top < s.y0 && s.y1 < *frame_bottom) return false;
        if (g <= 0.3 * text_h_ && neighbour.size() > 1) return false;
        // A title above a table of three or more columns starts at its left edge or is centred on
        // it; text over some columns in between is a group header. Below the table, a last row
        // with empty cells stays unless it reads as a note.
        if (leading && typical >= 3) {
            const double centre = (s.x0 + s.x1) / 2;
            return s.x0 - tx0 <= 1.5 * text_h_ || std::abs(centre - (tx0 + tx1) / 2) <= 0.1 * (tx1 - tx0) || s.h() > 1.15 * text_h_;
        }
        return g > 1.4 * med_gap + 2 || s.h() > 1.15 * text_h_ || s.x1 - s.x0 > 0.6 * (tx1 - tx0);
    };
    // A paragraph's lines go together: its short last line follows a long one closely.
    std::optional<Row> prev_para;
    while (rows.size() > 2) {
        const Row r = rows.front();
        const bool follows = prev_para && runs_of(r).size() == 1 && gap(*prev_para, r) <= 0.5 * text_h_;
        if (paragraph(r) || follows || context(r, gap(r, rows[1]), rows[1], true)) {
            prev_para = paragraph(r) || follows ? std::optional<Row>(r) : std::nullopt;
            rows.erase(rows.begin());
        } else {
            break;
        }
    }
    while (rows.size() > 2) {
        const Row &r = rows.back();
        const Row &before = rows[rows.size() - 2];
        const bool tail = runs_of(r).size() == 1 && paragraph(before) && gap(before, r) <= 0.5 * text_h_;
        if (paragraph(r) || tail || context(r, gap(before, r), before, false))
            rows.pop_back();
        else
            break;
    }
    return rows;
}

// Where the table starts and ends among the visual rows. The rules kept a run of them (kept);
// trees trained on screenshots with known cells weigh that with the measurements below, and the
// rows they leave out at either end (titles, captions, notes) are cut.
std::vector<Row> Builder::table_extent(const std::vector<Row> &all, const std::vector<Row> &kept, const std::function<int(const Row &)> &straddles) {
    const int n = static_cast<int>(all.size());
    int lo = -1, hi = -2;
    if (!kept.empty()) {
        const auto it = std::find(all.begin(), all.end(), kept.front());
        if (it == all.end()) return kept;
        lo = static_cast<int>(it - all.begin());
        hi = lo + static_cast<int>(kept.size()) - 1;
    }
    if (n == 0) return kept;
    const double t = text_h_;
    double tx0 = 1e18, tx1 = -1e18;
    for (const auto &r : all)
        for (int s : r) {
            tx0 = std::min(tx0, segs_[s].x0);
            tx1 = std::max(tx1, segs_[s].x1);
        }
    const double w = std::max(1.0, tx1 - tx0);
    std::vector<double> long_rules;
    for (const auto &h : hrules_)
        if (h.end - h.start >= 0.5 * w) long_rules.push_back(h.pos);
    std::sort(long_rules.begin(), long_rules.end());
    std::vector<double> gaps, seg_counts;
    for (int i = 0; i + 1 < n; ++i) gaps.push_back(gap(all[i], all[i + 1]));
    for (const auto &r : all) seg_counts.push_back(static_cast<double>(r.size()));
    const double med_gap = median(gaps), typical = median(seg_counts);
    const double nbounds = static_cast<double>(bounds_.size());
    static const QStringList note_marks = {"*", QString::fromUtf8("※"), QString::fromUtf8("注"), "Note", "Source", QString::fromUtf8("来源"),
                                           QString::fromUtf8("資料"), QString::fromUtf8("出典"), QString::fromUtf8("出所"), QString::fromUtf8("备注")};
    static const QStringList caption_marks = {"Table", QString::fromUtf8("表"), "Fig", QString::fromUtf8("图"), QString::fromUtf8("圖")};
    auto text_of = [&](const Row &r) {
        std::vector<int> order(r);
        std::sort(order.begin(), order.end(), [&](int a, int b) { return segs_[a].x0 < segs_[b].x0; });
        QString out;
        for (int s : order) out += segs_[s].text() + ' ';
        return out.trimmed();
    };
    auto rule_between = [&](double ya, double yb) {
        return std::any_of(long_rules.begin(), long_rules.end(), [&](double y) { return ya - 2 <= y && y <= yb + 2; });
    };
    std::vector<double> fills(n, -1);
    auto fill = [&](int i) {
        if (fills[i] < 0) fills[i] = fill_of(all[i], tx0, tx1);
        return fills[i];
    };
    std::vector<uint8_t> in(n, 0);
    std::vector<double> scores(n, 0);
    for (int i = 0; i < n; ++i) {
        const Row &r = all[i];
        ExtentRow row;
        auto &f = row.features;
        double rx0 = 1e18, rx1 = -1e18;
        std::vector<double> hs;
        for (int s : r) {
            rx0 = std::min(rx0, segs_[s].x0);
            rx1 = std::max(rx1, segs_[s].x1);
            hs.push_back(segs_[s].h());
        }
        f[0] = static_cast<float>(std::min(i, 6));
        f[1] = static_cast<float>(std::min(n - 1 - i, 6));
        f[2] = static_cast<float>(n);
        f[3] = static_cast<float>(r.size());
        f[4] = static_cast<float>((rx1 - rx0) / w);
        f[5] = static_cast<float>((rx0 - tx0) / t);
        f[6] = static_cast<float>((tx1 - rx1) / t);
        f[7] = static_cast<float>(std::abs((rx0 + rx1) / 2 - (tx0 + tx1) / 2) / w);
        f[8] = static_cast<float>(median(hs) / t);
        f[9] = static_cast<float>(*std::max_element(hs.begin(), hs.end()) / t);
        f[10] = static_cast<float>(i > 0 ? gaps[i - 1] / t : -9);
        f[11] = static_cast<float>(i + 1 < n ? gaps[i] / t : -9);
        f[12] = static_cast<float>(i > 0 ? (gaps[i - 1] - med_gap) / t : -9);
        f[13] = static_cast<float>(i + 1 < n ? (gaps[i] - med_gap) / t : -9);
        f[14] = static_cast<float>(straddles(r));
        int crossed = 0;
        for (double b : bounds_) {
            bool hit = false;
            for (int s : r)
                for (const auto &word : segs_[s].words)
                    if (word.x0 + 1 < b && b < word.x1 - 1) hit = true;
            crossed += hit;
        }
        f[15] = static_cast<float>(crossed);
        f[16] = static_cast<float>(nbounds);
        f[17] = static_cast<float>(row_cols(r).size() / (nbounds + 1));
        std::vector<PlacedWord> words;
        for (int s : r) words.insert(words.end(), segs_[s].words.begin(), segs_[s].words.end());
        std::stable_sort(words.begin(), words.end(), [](const PlacedWord &a, const PlacedWord &b) { return a.x0 < b.x0; });
        double longest = 0, start = words.front().x0, end = words.front().x1;
        int runs = 1;
        for (size_t k = 1; k < words.size(); ++k) {
            if (words[k].x0 - end > 0.6 * t) {
                longest = std::max(longest, end - start);
                start = words[k].x0;
                ++runs;
            }
            end = std::max(end, words[k].x1);
        }
        longest = std::max(longest, end - start);
        f[18] = static_cast<float>(longest / w);
        f[19] = static_cast<float>(runs);
        f[20] = rule_between(i > 0 ? bottom(all[i - 1]) : top(r) - 2 * t, top(r)) ? 1.f : 0.f;
        f[21] = rule_between(bottom(r), i + 1 < n ? top(all[i + 1]) : bottom(r) + 2 * t) ? 1.f : 0.f;
        const bool framed = long_rules.size() >= 2;
        f[22] = framed && long_rules.front() < top(r) && bottom(r) < long_rules.back() ? 1.f : 0.f;
        f[23] = framed && bottom(r) < long_rules.front() ? 1.f : 0.f;
        f[24] = framed && top(r) > long_rules.back() ? 1.f : 0.f;
        f[25] = std::any_of(vrules_.begin(), vrules_.end(), [&](const Rule &v) {
                    return tx0 - t < v.pos && v.pos < tx1 + t && v.start <= top(r) + 0.2 * t && v.end >= bottom(r) - 0.2 * t;
                }) ? 1.f : 0.f;
        f[26] = static_cast<float>(i + 1 < n ? std::abs(fill(i) - fill(i + 1)) : 0);
        f[27] = static_cast<float>(i > 0 ? std::abs(fill(i) - fill(i - 1)) : 0);
        row.rules_in = lo <= i && i <= hi;
        f[28] = row.rules_in ? 1.f : 0.f;
        const QString text = text_of(r);
        f[29] = !text.isEmpty() && (text.back() == ':' || text.back() == QChar(0xFF1A)) ? 1.f : 0.f;
        f[30] = !text.isEmpty() && (text.back() == '.' || text.back() == QChar(0x3002)) ? 1.f : 0.f;
        f[31] = std::any_of(note_marks.begin(), note_marks.end(), [&](const QString &m) { return text.startsWith(m, Qt::CaseInsensitive); }) ? 1.f : 0.f;
        f[32] = std::any_of(caption_marks.begin(), caption_marks.end(), [&](const QString &m) { return text.startsWith(m, Qt::CaseInsensitive); }) ? 1.f : 0.f;
        int digits = 0, cjk = 0, chars = 0;
        for (QChar c : text) {
            if (c.isSpace()) continue;
            ++chars;
            digits += c.isDigit();
            cjk += is_cjk(c);
        }
        f[33] = static_cast<float>(std::min(chars, 300));
        f[34] = static_cast<float>(chars ? static_cast<double>(digits) / chars : 0);
        f[35] = static_cast<float>(chars ? static_cast<double>(cjk) / chars : 0);
        f[36] = static_cast<float>(i > 0 ? static_cast<double>(all[i - 1].size()) : -1);
        f[37] = static_cast<float>(i + 1 < n ? static_cast<double>(all[i + 1].size()) : -1);
        const auto own = row_cols(r);
        auto shared_with = [&](int k) {
            const auto other = row_cols(all[k]);
            int m = 0;
            for (int c : own) m += static_cast<int>(other.count(c));
            return static_cast<float>(m / std::max<double>(1.0, static_cast<double>(own.size())));
        };
        f[38] = i + 1 < n ? shared_with(i + 1) : -1.f;
        f[39] = i > 0 ? shared_with(i - 1) : -1.f;
        f[40] = static_cast<float>(fill(i) / 255.0);
        f[41] = static_cast<float>(r.size() / std::max(1.0, typical));
        const int given = i < static_cast<int>(control_.given_extent.size()) ? control_.given_extent[i] : -1;
        if (extent_model::MODEL_TRAINED) scores[i] = extent_score(row.features);
        if (given == 0 || given == 1)
            row.in = given == 1;
        else if (control_.use_extent_model && extent_model::MODEL_TRAINED)
            row.in = scores[i] >= (row.rules_in ? -EXTENT_OVERRIDE_MARGIN : EXTENT_OVERRIDE_MARGIN) &&
                     // A segment running through several column boundaries fits no column: a title or
                     // prose the rules left out stays out.
                     (row.rules_in || f[14] < std::max(2.0, std::floor(nbounds / 3)));
        else
            row.in = row.rules_in;
        in[i] = row.in;
        if (control_.trace) {
            control_.trace->extent.append(row);
            QVector<QRectF> boxes;
            QStringList texts;
            for (int s : r) {
                boxes.append(QRectF(QPointF(segs_[s].x0, segs_[s].y0), QPointF(segs_[s].x1, segs_[s].y1)));
                texts.append(segs_[s].text());
            }
            control_.trace->all_rows.append(boxes);
            control_.trace->all_texts.append(texts);
        }
    }
    int first = 0, last = n - 1;
    while (first < n && !in[first]) ++first;
    while (last >= 0 && !in[last]) --last;
    if (first > last) {
        if (lo < 0) return kept;
        first = lo;
        last = hi;
    }

    // Title and note: the rows right above and below the table that trees trained on screenshots
    // with known titles and notes keep, outward from the table until one is not kept.
    double bx0 = 1e18, bx1 = -1e18;
    std::vector<double> body_h;
    for (int i = first; i <= last; ++i)
        for (int s : all[i]) {
            bx0 = std::min(bx0, segs_[s].x0);
            bx1 = std::max(bx1, segs_[s].x1);
            body_h.push_back(segs_[s].h());
        }
    const double bw = std::max(1.0, bx1 - bx0), body_med = std::max(1.0, median(body_h));
    const int height = image_.height();
    auto caption = [&](int k, bool above, int distance) {
        const Row &r = all[k];
        CaptionRow row;
        row.row = k;
        row.above = above;
        auto &f = row.features;
        const Row &edge = above ? all[first] : all[last];
        const Row &toward = above ? all[k + 1] : all[k - 1];
        double rx0 = 1e18, rx1 = -1e18;
        std::vector<double> hs;
        for (int s : r) {
            rx0 = std::min(rx0, segs_[s].x0);
            rx1 = std::max(rx1, segs_[s].x1);
            hs.push_back(segs_[s].h());
        }
        const QString text = text_of(r);
        int digits = 0, cjk = 0, chars = 0;
        for (QChar c : text) {
            if (c.isSpace()) continue;
            ++chars;
            digits += c.isDigit();
            cjk += is_cjk(c);
        }
        std::vector<PlacedWord> words;
        for (int s : r) words.insert(words.end(), segs_[s].words.begin(), segs_[s].words.end());
        std::stable_sort(words.begin(), words.end(), [](const PlacedWord &a, const PlacedWord &b) { return a.x0 < b.x0; });
        double longest = 0, start = words.front().x0, end = words.front().x1;
        int runs = 1;
        for (size_t m = 1; m < words.size(); ++m) {
            if (words[m].x0 - end > 0.6 * t) {
                longest = std::max(longest, end - start);
                start = words[m].x0;
                ++runs;
            }
            end = std::max(end, words[m].x1);
        }
        longest = std::max(longest, end - start);
        f[0] = above ? 1.f : 0.f;
        f[1] = static_cast<float>(distance);
        f[2] = static_cast<float>((above ? top(toward) - bottom(r) : top(r) - bottom(toward)) / t);
        f[3] = static_cast<float>((above ? top(edge) - bottom(r) : top(r) - bottom(edge)) / t);
        f[4] = static_cast<float>(r.size());
        f[5] = static_cast<float>((rx1 - rx0) / bw);
        f[6] = static_cast<float>((rx0 - bx0) / t);
        f[7] = static_cast<float>((bx1 - rx1) / t);
        f[8] = static_cast<float>(std::abs((rx0 + rx1) / 2 - (bx0 + bx1) / 2) / bw);
        f[9] = static_cast<float>(median(hs) / t);
        f[10] = static_cast<float>(*std::max_element(hs.begin(), hs.end()) / t);
        f[11] = static_cast<float>(median(hs) / body_med);
        f[12] = static_cast<float>(std::min(chars, 300));
        f[13] = static_cast<float>(chars ? static_cast<double>(digits) / chars : 0);
        f[14] = static_cast<float>(chars ? static_cast<double>(cjk) / chars : 0);
        f[15] = !text.isEmpty() && (text.back() == ':' || text.back() == QChar(0xFF1A)) ? 1.f : 0.f;
        f[16] = !text.isEmpty() && (text.back() == '.' || text.back() == QChar(0x3002)) ? 1.f : 0.f;
        f[17] = std::any_of(note_marks.begin(), note_marks.end(), [&](const QString &m) { return text.startsWith(m, Qt::CaseInsensitive); }) ? 1.f : 0.f;
        f[18] = std::any_of(caption_marks.begin(), caption_marks.end(), [&](const QString &m) { return text.startsWith(m, Qt::CaseInsensitive); }) ? 1.f : 0.f;
        f[19] = static_cast<float>(runs);
        f[20] = static_cast<float>(longest / bw);
        f[21] = static_cast<float>(straddles(r));
        f[22] = (above ? rule_between(bottom(r), top(edge)) : rule_between(bottom(edge), top(r))) ? 1.f : 0.f;
        f[23] = static_cast<float>(std::abs(fill(k) - fill(above ? first : last)));
        f[24] = static_cast<float>(above ? k : n - 1 - k);
        f[25] = static_cast<float>((above ? top(r) : height - bottom(r)) / t);
        f[26] = static_cast<float>(scores[k]);
        f[27] = lo <= k && k <= hi ? 1.f : 0.f;
        f[28] = static_cast<float>(above ? first : n - 1 - last);
        f[29] = static_cast<float>(last - first + 1);
        const int given = k < static_cast<int>(control_.given_caption.size()) ? control_.given_caption[k] : -1;
        if (given == 0 || given == 1)
            row.kept = given == 1;
        else
            row.kept = control_.use_caption_model && caption_model::MODEL_TRAINED && caption_score(row.features) >= CAPTION_KEEP_SCORE;
        if (control_.trace) control_.trace->captions.append(row);
        return row.kept;
    };
    title_.clear();
    note_.clear();
    bool open = true;
    for (int k = first - 1, d = 1; k >= 0 && d <= 4; --k, ++d) {
        open = caption(k, true, d) && open;
        if (open) title_.insert(title_.begin(), all[k]);
    }
    open = true;
    for (int k = last + 1, d = 1; k < n && d <= 3; ++k, ++d) {
        open = caption(k, false, d) && open;
        if (open) note_.push_back(all[k]);
    }
    return std::vector<Row>(all.begin() + first, all.begin() + last + 1);
}

// Background of a visual row: the median luma around its words from x0 to x1.
double Builder::fill_of(const Row &row, double x0, double x1) const {
    if (gray_.empty() || row.empty()) return 0;
    const int width = image_.width(), height = image_.height();
    const int ya = std::clamp(trunc_int(top(row)), 0, height), yb = std::clamp(trunc_int(bottom(row)) + 1, 0, height);
    const int xa = std::clamp(trunc_int(x0), 0, width), xb = std::clamp(trunc_int(x1) + 1, 0, width);
    std::array<int, 256> hist{};
    int count = 0;
    for (int y = ya; y < yb; ++y)
        for (int x = xa; x < xb; ++x) {
            bool inside = false;
            for (int s : row)
                if (segs_[s].x0 - 1 <= x && x <= segs_[s].x1 + 1) inside = true;
            if (inside) continue;
            ++hist[gray_[static_cast<size_t>(y) * width + x]];
            ++count;
        }
    if (!count) return 0;
    int seen = 0;
    for (int v = 0; v < 256; ++v) {
        seen += hist[v];
        if (2 * seen >= count) return v;
    }
    return 255;
}

QString Builder::join_text(std::vector<int> parts) const {
    const double unit = std::max(1.0, text_h_ * 0.6);
    std::stable_sort(parts.begin(), parts.end(), [&](int a, int b) {
        const double ra = std::nearbyint(segs_[a].yc() / unit), rb = std::nearbyint(segs_[b].yc() / unit);
        return ra != rb ? ra < rb : segs_[a].x0 < segs_[b].x0;
    });
    QString text;
    for (int p : parts) {
        const QString t = segs_[p].text();
        if (!text.isEmpty() && !(ends_cjk(text) && starts_cjk(t))) text += ' ';
        text += t;
    }
    return text;
}

Table Builder::build() {
    // Past a few thousand segments the screenshot is a page of text, not a table.
    if (segs_.empty() || segs_.size() > 3000) return {};
    std::vector<double> heights;
    for (const auto &s : segs_) heights.push_back(s.h());
    text_h_ = median(heights);
    detect_rules();
    const auto all = visual_rows();
    auto rows = drop_context(all);

    // Prose lines have gaps like merged cells, so the first pass counts only rows of several segments.
    find_columns(rows, false);
    // Prose above or below the table runs through column boundaries; its short last line goes with it.
    // Counted per segment: group headers each centred over two columns cross one boundary apiece.
    auto straddles = [&](const Row &row) {
        int most = 0;
        for (int s : row) {
            int n = 0;
            for (double b : bounds_)
                n += std::any_of(segs_[s].words.begin(), segs_[s].words.end(), [&](const PlacedWord &w) { return w.x0 + 1 < b && b < w.x1 - 1; });
            most = std::max(most, n);
        }
        return most;
    };
    auto one_run = [&](const Row &row) {
        std::vector<PlacedWord> words;
        for (int s : row) words.insert(words.end(), segs_[s].words.begin(), segs_[s].words.end());
        std::stable_sort(words.begin(), words.end(), [](const PlacedWord &a, const PlacedWord &b) { return a.x0 < b.x0; });
        for (size_t k = 0; k + 1 < words.size(); ++k)
            if (words[k + 1].x0 - words[k].x1 > 1.2 * text_h_) return false;
        return true;
    };
    const int need = std::max(2, static_cast<int>(bounds_.size()) / 3);
    while (rows.size() > 2 && !bounds_.empty()) {
        if (straddles(rows.front()) >= need) {
            rows.erase(rows.begin());
            while (rows.size() > 2 && one_run(rows.front()) && straddles(rows.front()) >= 1 && gap(rows[0], rows[1]) > 0) rows.erase(rows.begin());
        } else if (straddles(rows.back()) >= need ||
                   (rows.size() > 3 && one_run(rows.back()) && straddles(rows[rows.size() - 2]) >= need && gap(rows[rows.size() - 2], rows.back()) <= 0.5 * text_h_)) {
            rows.pop_back();
        } else {
            break;
        }
    }
    rows = table_extent(all, rows, straddles);
    if (rows.empty()) return {};
    find_columns(rows, true);
    double tx0 = 1e18, tx1 = -1e18, ty0 = 1e18, ty1 = -1e18;
    for (const auto &r : rows)
        for (int s : r) {
            tx0 = std::min(tx0, segs_[s].x0);
            tx1 = std::max(tx1, segs_[s].x1);
            ty0 = std::min(ty0, segs_[s].y0);
            ty1 = std::max(ty1, segs_[s].y1);
        }
    const double twidth = tx1 - tx0;

    // Split segments at column boundaries where a word gap falls on one: two cells read as one line.
    for (auto &r : rows) {
        Row out;
        for (int s : r) {
            std::vector<int> pieces{s};
            for (double b : bounds_) {
                const int last = pieces.back();
                if (segs_[last].x0 < b && b < segs_[last].x1) {
                    const int first = split(last, b, 0.4 * text_h_);
                    if (first >= 0) {
                        pieces.back() = first;
                        pieces.push_back(first + 1);
                    }
                }
            }
            out.insert(out.end(), pieces.begin(), pieces.end());
        }
        r = std::move(out);
    }
    const int ncols = static_cast<int>(bounds_.size()) + 1;
    const int full = std::max(2, (ncols + 1) / 2);
    const int n = static_cast<int>(rows.size());
    std::vector<ColumnSet> cols;
    for (const auto &r : rows) cols.push_back(row_cols(r));
    auto shared = [](const ColumnSet &a, const ColumnSet &b) {
        int k = 0;
        for (int c : a) k += static_cast<int>(b.count(c));
        return k;
    };

    // Rule mode: horizontal rules separate the rows.
    std::vector<int> anchors;
    for (int i = 0; i < n; ++i)
        if (static_cast<int>(cols[i].size()) >= full) anchors.push_back(i);
    std::set<double> long_rules;
    for (const auto &h : hrules_)
        if (ty0 < h.pos && h.pos < ty1 && h.end - h.start >= 0.5 * twidth) long_rules.insert(std::nearbyint(h.pos));
    const bool rule_mode = long_rules.size() >= 2 && long_rules.size() >= 0.5 * (static_cast<double>(anchors.size()) - 1);
    // Spacing between consecutive rows that fill the same columns, trusted only when most rows
    // are single lines: wrapped cells make full rows share a row.
    std::vector<double> anchor_gaps;
    for (size_t k = 0; k + 1 < anchors.size(); ++k) {
        const int a = anchors[k], b = anchors[k + 1];
        if (b == a + 1 && shared(cols[a], cols[b]) >= full && gap(rows[a], rows[b]) > 0) anchor_gaps.push_back(gap(rows[a], rows[b]));
    }
    std::sort(anchor_gaps.begin(), anchor_gaps.end());
    std::optional<double> row_gap;
    if (static_cast<double>(anchor_gaps.size()) >= std::max(3.0, 0.5 * (n - 1))) row_gap = anchor_gaps[anchor_gaps.size() / 2];

    auto ruled = [&](const Row &a, const Row &b) { return hrule_between(bottom(a), top(b), tx0, tx1, 1.5 * text_h_); };
    std::map<int, double> widest;
    for (const auto &r : rows)
        for (int s : r) {
            const auto [c0, c1] = seg_cols(s);
            if (c0 == c1) widest[c0] = std::max(widest[c0], segs_[s].x1 - segs_[s].x0);
        }
    // A group header over its columns: a segment of u spans two or more columns that l fills, or
    // u has fewer cells than l, all in columns l fills.
    auto header_over = [&](const Row &u, const Row &l) {
        const auto lc = row_cols(l);
        // b is a's next line in the same cell: same column, an edge or the centre aligned, and a
        // about as wide as the column's widest text, as a line that wraps on is.
        auto continues = [&](int a, int b) {
            const auto ca = seg_cols(a);
            const auto it = widest.find(ca.first);
            if (ca != seg_cols(b) || segs_[a].x1 - segs_[a].x0 < 0.75 * (it == widest.end() ? 0.0 : it->second)) return false;
            const double tol = 0.3 * text_h_;
            return std::abs(segs_[a].x0 - segs_[b].x0) <= tol || std::abs(segs_[a].x1 - segs_[b].x1) <= tol ||
                   std::abs((segs_[a].x0 + segs_[a].x1) / 2 - (segs_[b].x0 + segs_[b].x1) / 2) <= tol;
        };
        bool cont = false;
        for (int a : u)
            for (int b : l)
                if (continues(a, b)) cont = true;
        if (cont && gap(u, l) <= 0.5 * text_h_) return false;
        const auto uc = row_cols(u);
        if (u.size() < l.size() && std::includes(lc.begin(), lc.end(), uc.begin(), uc.end())) return true;
        // Judged on the segment as read, before splitting at column boundaries.
        std::set<int> origins;
        for (int s : u) origins.insert(segs_[s].origin);
        for (int o : origins) {
            const auto [c0, c1] = seg_cols(o);
            int k = 0;
            for (int c = c0; c <= c1; ++c) k += static_cast<int>(lc.count(c));
            if (k >= 2) return true;
        }
        return false;
    };
    // Row i sits between rows i-1 and i+1 in columns neither uses: text centred over a row span.
    auto between = [&](int i) {
        if (i == 0 || i + 1 >= n) return false;
        const Row &u = rows[i - 1], &m = rows[i], &l = rows[i + 1];
        if (shared(cols[i], cols[i - 1]) || shared(cols[i], cols[i + 1])) return false;
        const double mc = (top(m) + bottom(m)) / 2;
        if (!(top(u) < mc && mc < bottom(l))) return false;
        if (gap(u, m) > 0.6 * text_h_ || gap(m, l) > 0.6 * text_h_ || ruled(u, m) || ruled(m, l)) return false;
        // Few cells between two fuller rows span both. Otherwise the cells stand beside a group
        // header and its columns, or between two rows, or are the middle line of wrapped cells,
        // whose lines above and below are close.
        if (cols[i].size() < cols[i - 1].size() && cols[i].size() < cols[i + 1].size()) return true;
        return header_over(u, l) || gap(u, l) > 0.7 * text_h_;
    };

    std::vector<int> link(n);
    std::iota(link.begin(), link.end(), 0);
    auto ufind = [&](int i) {
        while (link[i] != i) {
            link[i] = link[link[i]];
            i = link[i];
        }
        return i;
    };
    auto unite = [&](int i, int j) {
        const int a = ufind(i), b = ufind(j);
        if (a != b) link[std::max(a, b)] = std::min(a, b);
    };

    std::set<int> down, split_after, header_rows;
    std::map<int, std::pair<int, int>> forced;
    for (int i = 1; i + 1 < n; ++i) {
        if (!between(i)) continue;
        if (header_over(rows[i - 1], rows[i + 1])) header_rows.insert(i + 1);
        down.insert(rows[i].begin(), rows[i].end());
        split_after.insert(i);
        // A header above the row-spanning cells covers the columns those cells leave free. Each free
        // column goes to the nearest header of that row, and none across a vertical rule beside it:
        // two group headers never take the same column.
        auto free = [&](int c) { return c >= 0 && c < ncols && !cols[i].count(c) && cols[i + 1].count(c) && !cols[i - 1].count(c); };
        const Row &upper = rows[i - 1];
        const double ry0 = top(upper) + 0.2 * text_h_, ry1 = bottom(upper) - 0.2 * text_h_;
        auto nearest = [&](int c) {
            const double lo = c == 0 ? tx0 : bounds_[c - 1], hi = c + 1 == ncols ? tx1 : bounds_[c];
            const double x = (lo + hi) / 2;
            int best = -1;
            double best_d = 1e18;
            for (int s : upper) {
                const double d = x < segs_[s].x0 ? segs_[s].x0 - x : x > segs_[s].x1 ? x - segs_[s].x1 : 0;
                if (d < best_d) {
                    best_d = d;
                    best = s;
                }
            }
            return best;
        };
        for (int u : upper) {
            auto [c0, c1] = seg_cols(u);
            while (free(c0 - 1) && nearest(c0 - 1) == u && !vrule_at(bounds_[c0 - 1], ry0, ry1)) --c0;
            while (free(c1 + 1) && nearest(c1 + 1) == u && !vrule_at(bounds_[c1], ry0, ry1)) ++c1;
            forced[u] = {c0, c1};
        }
    }

    // Lines of one cell: consecutive segments of a column closer than the row spacing, one of
    // their rows partial. Rows whose middle falls within such a block share its row.
    std::map<int, int> row_of;
    for (int i = 0; i < n; ++i)
        for (int s : rows[i]) row_of[s] = i;
    std::vector<int> occ;
    for (const auto &c : cols) occ.push_back(static_cast<int>(c.size()));
    std::map<int, std::vector<int>> percol;
    for (const auto &r : rows)
        for (int s : r) {
            const auto [c0, c1] = seg_cols(s);
            if (c0 == c1) percol[c0].push_back(s);
        }
    // With rules between rows, lines without one between them may be further apart.
    double base = 0.6 * text_h_;
    if (!rule_mode && row_gap) base = std::min(base, *row_gap - 1.5);
    std::vector<std::pair<int, int>> pairs;
    for (auto &[c, ss] : percol) {
        std::stable_sort(ss.begin(), ss.end(), [this](int a, int b) { return segs_[a].y0 < segs_[b].y0; });
        struct Pair {
            double gap;
            int a, b;
        };
        std::vector<Pair> col_pairs;
        for (size_t k = 0; k + 1 < ss.size(); ++k) {
            const int a = ss[k], b = ss[k + 1];
            const int ia = row_of[a], ib = row_of[b];
            // No cell continues past a header row, or across cells centred over a row span.
            if (header_rows.count(ia) && ib > ia) continue;
            if (std::any_of(split_after.begin(), split_after.end(), [&](int m) { return ia < m && m < ib; })) continue;
            // Two full rows share a row only in a ruled table, with the lines pressed together
            // (header cells wrapped onto a second line).
            const bool partial = occ[ia] < full || occ[ib] < full;
            const bool pressed = rule_mode && segs_[b].y0 - segs_[a].y1 <= 0.25 * text_h_;
            if (ia != ib && (partial || pressed) && !ruled(Row{a}, Row{b})) col_pairs.push_back({segs_[b].y0 - segs_[a].y1, a, b});
        }
        // Two clusters of gaps in a column: lines within a cell, and cells of successive rows.
        // Per script too, as line boxes of CJK and Latin text leave different gaps.
        for (bool script : {true, false}) {
            std::vector<Pair> group;
            for (const auto &p : col_pairs)
                if (has_cjk(segs_[p.a].text()) == script) group.push_back(p);
            double limit = base;
            std::vector<double> small;
            for (const auto &p : group)
                if (p.gap <= limit) small.push_back(p.gap);
            std::sort(small.begin(), small.end());
            // The lower cluster, lines within cells, needs two members to count as one.
            std::optional<std::tuple<double, double, double>> jump;
            for (size_t k = 1; k + 1 < small.size(); ++k) {
                const double lo = small[k], hi = small[k + 1];
                if (hi - lo < 2) continue;
                // Overlapping lines have negative gaps; the ratio counts them as touching.
                const auto candidate = std::make_tuple((std::max(hi, 0.0) + 2) / (std::max(lo, 0.0) + 2), lo, hi);
                if (!jump || candidate > *jump) jump = candidate;
            }
            if (jump && !rule_mode && std::get<0>(*jump) >= 1.3) limit = (std::get<1>(*jump) + std::get<2>(*jump)) / 2;
            for (const auto &p : group)
                if (p.gap <= limit) pairs.push_back({p.a, p.b});
        }
    }
    // Blocks: segments joined by those pairs.
    std::vector<int> bparent(segs_.size());
    std::iota(bparent.begin(), bparent.end(), 0);
    std::vector<uint8_t> in_block(segs_.size(), 0);
    auto bfind = [&](int i) {
        while (bparent[i] != i) {
            bparent[i] = bparent[bparent[i]];
            i = bparent[i];
        }
        return i;
    };
    for (const auto &[a, b] : pairs) {
        in_block[a] = in_block[b] = 1;
        const int ra = bfind(a), rb = bfind(b);
        if (ra != rb) bparent[std::max(ra, rb)] = std::min(ra, rb);
    }
    auto same_block = [&](int x, int y) { return in_block[x] && in_block[y] && bfind(x) == bfind(y); };
    std::map<int, std::vector<int>> block_members;
    for (int s = 0; s < static_cast<int>(segs_.size()); ++s)
        if (in_block[s]) block_members[bfind(s)].push_back(s);
    auto span_rows = [&](const std::vector<int> &blk) {
        double t = 1e18, b = -1e18;
        for (int x : blk) {
            t = std::min(t, segs_[x].y0);
            b = std::max(b, segs_[x].y1);
        }
        std::vector<int> idx;
        for (int i = 0; i < n; ++i) {
            const double mid = (top(rows[i]) + bottom(rows[i])) / 2;
            if (t - 0.1 * text_h_ <= mid && mid <= b + 0.1 * text_h_) idx.push_back(i);
        }
        return idx;
    };
    // Another column holds cells of two rows within the block's span, not lines of one cell.
    auto two_rows = [&](const std::vector<int> &blk, const std::vector<int> &idx) {
        const std::set<int> own(blk.begin(), blk.end());
        std::map<int, std::vector<std::pair<int, int>>> seen;
        for (int i : idx)
            for (int x : rows[i]) {
                if (own.count(x)) continue;
                const auto [c0, c1] = seg_cols(x);
                for (int c = c0; c <= c1; ++c) {
                    for (const auto &[j, y] : seen[c])
                        if (j != i && !same_block(x, y)) return true;
                    seen[c].push_back({i, x});
                }
            }
        return false;
    };
    // A block over two rows took a cell of the other row (a header over the first wrapped line):
    // it splits at its widest gap that is not two lines pressed together.
    std::vector<std::vector<int>> pending;
    for (auto &[root, members] : block_members) {
        std::stable_sort(members.begin(), members.end(), [this](int a, int b) { return segs_[a].y0 < segs_[b].y0; });
        pending.push_back(members);
    }
    while (!pending.empty()) {
        const auto blk = pending.back();
        pending.pop_back();
        if (blk.size() < 2) continue;
        const auto idx = span_rows(blk);
        if (two_rows(blk, idx)) {
            std::optional<std::pair<double, size_t>> cut;
            for (size_t k = 0; k + 1 < blk.size(); ++k) {
                const double g = segs_[blk[k + 1]].y0 - segs_[blk[k]].y1;
                if (g > 0.25 * text_h_ && (!cut || std::make_pair(g, k) > *cut)) cut = std::make_pair(g, k);
            }
            if (cut) {
                pending.emplace_back(blk.begin(), blk.begin() + static_cast<long>(cut->second) + 1);
                pending.emplace_back(blk.begin() + static_cast<long>(cut->second) + 1, blk.end());
                continue;
            }
        }
        for (int i : idx) unite(idx.front(), i);
    }
    for (int i : split_after) unite(i - 1, i);

    // Lines between the same two rules share a row: a cell wrapped onto more lines than its
    // neighbours, the single-line cells beside it centred. Only for lines closer than rows are to
    // each other, as a missed rule leaves two rows. The table's outer border counts as a rule.
    std::set<double> rule_set;
    for (const auto &h : hrules_)
        if (ty0 - 1.5 * text_h_ < h.pos && h.pos < ty1 + 1.5 * text_h_ && h.end - h.start >= 0.5 * twidth) rule_set.insert(std::nearbyint(h.pos));
    const std::vector<double> rule_ys(rule_set.begin(), rule_set.end());
    const int nbands = static_cast<int>(rule_ys.size());
    auto band_of = [&](double y) { return static_cast<int>(std::count_if(rule_ys.begin(), rule_ys.end(), [&](double r) { return r < y; })); };
    std::vector<int> bands;
    std::map<int, int> per_band;
    for (const auto &r : rows) {
        bands.push_back(band_of((top(r) + bottom(r)) / 2));
        ++per_band[bands.back()];
    }
    // A shorter rule inside a band divides spanned header cells; those rows stay as they are. It
    // runs across a whole column in the space between lines; the box around inline code or a tag
    // hugs its text.
    std::vector<double> col_edges{tx0};
    col_edges.insert(col_edges.end(), bounds_.begin(), bounds_.end());
    col_edges.push_back(tx1);
    std::vector<int> tsegs;
    for (const auto &r : rows) tsegs.insert(tsegs.end(), r.begin(), r.end());
    // One edge of a box around a line: a matching rule on the other side of the same line. A grid
    // line broken by a descender also frames a line, but ends at a vertical rule crossing it; the
    // box's own sides end at it.
    auto hugs = [&](const Rule &h) {
        for (const auto &v : vrules_)
            if ((std::abs(v.pos - h.start) <= 3 || std::abs(v.pos - h.end) <= 3) && v.start < h.pos - 3 && v.end > h.pos + 3) return false;
        for (int x : tsegs) {
            const auto &s = segs_[x];
            if (!(s.x0 - 0.8 * text_h_ <= h.start && h.end <= s.x1 + 0.8 * text_h_)) continue;
            const double mid = (s.y0 + s.y1) / 2;
            const bool above = s.y0 - 0.25 * text_h_ < h.pos && h.pos <= mid;
            const bool below = mid <= h.pos && h.pos < s.y1 + 0.25 * text_h_;
            if (!above && !below) continue;
            for (const auto &o : hrules_)
                if ((std::abs(o.start - h.start) <= 3 || std::abs(o.end - h.end) <= 3) &&
                    ((above && mid < o.pos && o.pos < s.y1 + 0.25 * text_h_) || (below && s.y0 - 0.25 * text_h_ < o.pos && o.pos < mid)))
                    return true;
        }
        return false;
    };
    std::vector<Rule> separators;
    std::set<int> divided;
    for (const auto &h : hrules_) {
        const bool box = hugs(h);
        if (!box) separators.push_back(h);
        if (box || !(ty0 < h.pos && h.pos < ty1) || h.end - h.start < 1.5 * text_h_) continue;
        if (std::any_of(rule_ys.begin(), rule_ys.end(), [&](double y) { return std::abs(h.pos - y) <= 3; })) continue;
        bool across = false;
        for (size_t k = 0; k + 1 < col_edges.size(); ++k)
            if (std::min<double>(h.end, col_edges[k + 1]) - std::max<double>(h.start, col_edges[k]) >= 0.8 * (col_edges[k + 1] - col_edges[k])) across = true;
        if (across) divided.insert(band_of(h.pos - 3));
    }
    // Sparse rules (above and below the header only, or every few rows) leave bands of many lines;
    // rules between rows leave bands of one row, a few lines when cells wrap.
    std::vector<int> closed;
    for (const auto &[b, count] : per_band)
        if (0 < b && b < nbands) closed.push_back(count);
    std::sort(closed.begin(), closed.end());
    if (closed.size() >= 3 && closed[closed.size() / 2] <= 3) {
        // Only bands closed by a rule on both sides: rules missed at the top or bottom (a header
        // fill matching the first row) leave open bands. A line crossing a rule spans rows.
        std::vector<uint8_t> crosses;
        for (const auto &r : rows)
            crosses.push_back(std::any_of(rule_ys.begin(), rule_ys.end(), [&](double y) { return top(r) + 2 < y && y < bottom(r) - 2; }));
        // A group header over two or more columns that later lines of its band fill heads a second
        // header row.
        for (int i = 0; i < n; ++i)
            for (int u : rows[i]) {
                const auto [c0, c1] = seg_cols(u);
                if (c1 <= c0) continue;
                for (int j = i + 1; j < n; ++j)
                    if (bands[j] == bands[i] &&
                        std::count_if(cols[j].begin(), cols[j].end(), [&](int c) { return c0 <= c && c <= c1; }) >= 2)
                        divided.insert(bands[i]);
            }
        // In a wrapped row a cell that starts below the band's first line is a single line centred
        // in the row; one lower down is a second header row under a group header.
        std::map<int, std::vector<int>> members;
        for (int i = 0; i < n; ++i) members[bands[i]].push_back(i);
        for (const auto &[b, idx] : members) {
            if (idx.size() < 2) continue;
            double t = 1e18, btm = -1e18;
            for (int i : idx) {
                t = std::min(t, top(rows[i]));
                btm = std::max(btm, bottom(rows[i]));
            }
            const double mid = (t + btm) / 2;
            ColumnSet seen = cols[idx.front()];
            for (size_t k = 1; k < idx.size(); ++k) {
                for (int x : rows[idx[k]]) {
                    const auto [c0, c1] = seg_cols(x);
                    bool known = false;
                    for (int c = c0; c <= c1; ++c) known = known || seen.count(c);
                    if (!known && std::abs(segs_[x].yc() - mid) > 0.35 * text_h_) divided.insert(b);
                }
                seen.insert(cols[idx[k]].begin(), cols[idx[k]].end());
            }
        }
        std::vector<double> ruled_gaps;
        for (int i = 1; i < n; ++i)
            if (bands[i] != bands[i - 1]) ruled_gaps.push_back(gap(rows[i - 1], rows[i]));
        std::sort(ruled_gaps.begin(), ruled_gaps.end());
        const double ruled_gap = ruled_gaps.empty() ? 0 : ruled_gaps[ruled_gaps.size() / 2];
        auto separated = [&](double ya, double yb) {
            for (const auto &h : separators)
                if (ya - 2 <= h.pos && h.pos <= yb + 2 && std::min<double>(h.end, tx1) - std::max<double>(h.start, tx0) >= 1.5 * text_h_) return true;
            return false;
        };
        // Line boxes padded until rows touch (PP-OCR's) leave no spacing to tell lines from rows.
        const int last = ruled_gap > 0.2 * text_h_ ? n : 1;
        for (int i = 1; i < last; ++i) {
            if (bands[i] != bands[i - 1] || divided.count(bands[i]) || !(0 < bands[i] && bands[i] < nbands) || crosses[i] || crosses[i - 1]) continue;
            // Cells centred between two lines span two rows, unless those lines are the only ones
            // beside them: a single wrapped cell.
            const int k = split_after.count(i) ? i : split_after.count(i - 1) ? i - 1 : -1;
            if (k >= 0 && !(static_cast<int>(cols[k - 1].size()) < full && static_cast<int>(cols[k + 1].size()) < full)) continue;
            const double g = gap(rows[i - 1], rows[i]);
            // Boxes around inline code or tags between them separate nothing.
            if ((g <= 0.1 * text_h_ || g < 0.7 * ruled_gap) && !separated(bottom(rows[i - 1]), top(rows[i]))) {
                unite(i - 1, i);
                // Centred between the lines of a wrapped cell, not over two rows.
                for (int m : {i - 1, i})
                    for (int x : rows[m]) down.erase(x);
            }
        }
    }

    // Whether each visual row continues the table row above it. The rules above give one answer;
    // trees trained on screenshots with known cells weigh it with the measurements below.
    std::vector<double> pair_gaps;
    for (int i = 0; i + 1 < n; ++i) pair_gaps.push_back(gap(rows[i], rows[i + 1]));
    const double med_pair_gap = median(pair_gaps);
    // Where a gap falls among the table's gaps: lines within cells and rows of the table form two
    // clusters, and in a table of wrapped cells the median is a line gap.
    std::vector<double> sorted_gaps(pair_gaps);
    std::sort(sorted_gaps.begin(), sorted_gaps.end());
    const double gap_p80 = sorted_gaps.empty() ? 0 : sorted_gaps[std::min(sorted_gaps.size() - 1, sorted_gaps.size() * 4 / 5)];
    double split_gap = sorted_gaps.empty() ? 0 : sorted_gaps.back(), split_ratio = 1;
    {
        double best = -1;
        for (size_t k = 1; k < sorted_gaps.size(); ++k) {
            const double lo = std::accumulate(sorted_gaps.begin(), sorted_gaps.begin() + static_cast<long>(k), 0.0) / static_cast<double>(k);
            const double hi = std::accumulate(sorted_gaps.begin() + static_cast<long>(k), sorted_gaps.end(), 0.0) / static_cast<double>(sorted_gaps.size() - k);
            const double separation = static_cast<double>(k) * static_cast<double>(sorted_gaps.size() - k) * (hi - lo) * (hi - lo);
            if (separation > best) {
                best = separation;
                split_gap = (sorted_gaps[k - 1] + sorted_gaps[k]) / 2;
                split_ratio = (hi - lo) / text_h_;
            }
        }
    }
    std::optional<double> head_rule;
    for (double y : rule_ys)
        if (ty0 + 0.5 * text_h_ < y && y < ty1 - 0.5 * text_h_) {
            int below = 0;
            for (const auto &r : rows) below += top(r) > y;
            if (below >= 2) {
                head_rule = y;
                break;
            }
        }
    std::vector<double> fills(n, -1);
    auto row_fill = [&](int i) {
        if (fills[i] < 0) fills[i] = fill_of(rows[i], tx0, tx1);
        return fills[i];
    };
    auto share = [](const std::vector<int> &segments, const std::function<bool(int)> &test) {
        if (segments.empty()) return 0.0;
        return static_cast<double>(std::count_if(segments.begin(), segments.end(), test)) / static_cast<double>(segments.size());
    };
    static const QRegularExpression numeric(QString::fromUtf8("^[-+±−$€£¥￥]?[\\d.,，．%‰ ]+[%‰]?$"));
    static const QString continuation = QString::fromUtf8("-,(/、，（&");
    static const QString opening = QString::fromUtf8("([（【「");
    // The segment of row r in column c, when it fills that column alone.
    auto seg_in = [&](int r, int c) -> int {
        for (int s : rows[r]) {
            const auto [c0, c1] = seg_cols(s);
            if (c0 == c && c1 == c) return s;
        }
        return -1;
    };
    std::vector<uint8_t> joined(std::max(0, n - 1), 0);
    for (int i = 0; i + 1 < n; ++i) {
        const int j = i + 1;
        const Row &u = rows[i], &l = rows[j];
        RowPair pair;
        auto &f = pair.features;
        const double t = text_h_;
        const double g = pair_gaps[i];
        f[0] = static_cast<float>(g / t);
        f[1] = static_cast<float>(i > 0 ? pair_gaps[i - 1] / t : -9);
        f[2] = static_cast<float>(j + 1 < n ? pair_gaps[j] / t : -9);
        f[3] = static_cast<float>((g - med_pair_gap) / t);
        f[4] = static_cast<float>(row_gap ? (g - *row_gap) / t : -9);
        double gmin = 9 * t, gmax = -9 * t;
        bool any_shared = false;
        int cont = 0, aligned = 0, centred = 0, single_l = 0;
        for (int b : l) {
            const auto [c0, c1] = seg_cols(b);
            if (c0 != c1) continue;
            ++single_l;
            const int a = seg_in(i, c0);
            if (a < 0) continue;
            any_shared = true;
            const double cg = segs_[b].y0 - segs_[a].y1;
            gmin = std::min(gmin, cg);
            gmax = std::max(gmax, cg);
            const auto it = widest.find(c0);
            const bool wide = segs_[a].x1 - segs_[a].x0 >= 0.75 * (it == widest.end() ? 0.0 : it->second);
            const bool left = std::abs(segs_[a].x0 - segs_[b].x0) <= 0.3 * t;
            const bool right = std::abs(segs_[a].x1 - segs_[b].x1) <= 0.3 * t;
            const bool centre = std::abs(segs_[a].x0 + segs_[a].x1 - segs_[b].x0 - segs_[b].x1) <= 0.6 * t;
            cont += wide && (left || right || centre);
            aligned += left;
            centred += centre;
        }
        f[5] = static_cast<float>(any_shared ? gmin / t : 9);
        f[6] = static_cast<float>(any_shared ? gmax / t : 9);
        std::vector<double> hu, hl;
        for (int s : u) hu.push_back(segs_[s].h());
        for (int s : l) hl.push_back(segs_[s].h());
        f[7] = static_cast<float>(median(hu) / t);
        f[8] = static_cast<float>(median(hl) / t);
        const auto &cu = cols[i], &cl = cols[j];
        const double nc = ncols;
        const int common = shared(cu, cl);
        f[9] = static_cast<float>(cu.size() / nc);
        f[10] = static_cast<float>(cl.size() / nc);
        f[11] = static_cast<float>(common / nc);
        f[12] = std::includes(cu.begin(), cu.end(), cl.begin(), cl.end()) ? 1.f : 0.f;
        f[13] = std::includes(cl.begin(), cl.end(), cu.begin(), cu.end()) ? 1.f : 0.f;
        f[14] = static_cast<float>((static_cast<double>(cl.size()) - common) / nc);
        f[15] = static_cast<float>((static_cast<double>(cu.size()) - common) / nc);
        f[16] = static_cast<float>(u.size());
        f[17] = static_cast<float>(l.size());
        f[18] = static_cast<float>(ncols);
        f[19] = ruled(u, l) ? 1.f : 0.f;
        f[20] = hrule_between(bottom(u), top(l), tx0, tx1, 0.5 * twidth) ? 1.f : 0.f;
        f[21] = rule_mode ? 1.f : 0.f;
        f[22] = lattice_ ? 1.f : 0.f;
        f[23] = nbands ? (bands[i] == bands[j] ? 1.f : 0.f) : -1.f;
        f[24] = static_cast<float>(nbands ? per_band[bands[i]] : 0);
        f[25] = static_cast<float>(single_l ? static_cast<double>(cont) / single_l : 0);
        std::vector<int> single_u;
        for (int s : u)
            if (seg_cols(s).first == seg_cols(s).second) single_u.push_back(s);
        f[26] = static_cast<float>(share(single_u, [&](int s) {
            const auto it = widest.find(seg_cols(s).first);
            return segs_[s].x1 - segs_[s].x0 >= 0.75 * (it == widest.end() ? 0.0 : it->second);
        }));
        f[27] = static_cast<float>(single_l ? static_cast<double>(aligned) / single_l : 0);
        f[28] = static_cast<float>(std::min(i, 6));
        f[29] = static_cast<float>(n > 1 ? static_cast<double>(i) / (n - 1) : 0);
        f[30] = head_rule && bottom(l) < *head_rule ? 1.f : 0.f;
        pair.rules_join = ufind(i) == ufind(j);
        f[31] = pair.rules_join ? 1.f : 0.f;
        f[32] = header_over(u, l) ? 1.f : 0.f;
        f[33] = std::any_of(pairs.begin(), pairs.end(), [&](const std::pair<int, int> &p) { return row_of[p.first] == i && row_of[p.second] == j; }) ? 1.f : 0.f;
        std::vector<int> both(u);
        both.insert(both.end(), l.begin(), l.end());
        f[34] = static_cast<float>(share(both, [&](int s) { return has_cjk(segs_[s].text()); }));
        f[35] = static_cast<float>(share(u, [&](int s) {
            const QString x = segs_[s].text();
            return !x.isEmpty() && continuation.contains(x.back());
        }));
        f[36] = static_cast<float>(share(l, [&](int s) {
            const QString x = segs_[s].text();
            return !x.isEmpty() && x.front().isLower();
        }));
        f[37] = static_cast<float>(share(l, [&](int s) {
            const QString x = segs_[s].text();
            return !x.isEmpty() && opening.contains(x.front());
        }));
        f[38] = static_cast<float>(share(u, [&](int s) { return numeric.match(segs_[s].text()).hasMatch(); }));
        f[39] = static_cast<float>(share(l, [&](int s) { return numeric.match(segs_[s].text()).hasMatch(); }));
        f[40] = static_cast<float>(std::abs(row_fill(i) - row_fill(j)));
        f[41] = static_cast<float>(n);
        f[42] = static_cast<float>(single_l ? static_cast<double>(centred) / single_l : 0);
        f[43] = static_cast<float>(std::lower_bound(sorted_gaps.begin(), sorted_gaps.end(), g) - sorted_gaps.begin()) / static_cast<float>(sorted_gaps.size());
        f[44] = static_cast<float>((g - gap_p80) / t);
        f[45] = static_cast<float>((g - split_gap) / t);
        f[46] = static_cast<float>(split_ratio);
        double neighbour = 9 * t;
        if (i > 0) neighbour = std::min(neighbour, pair_gaps[i - 1]);
        if (j + 1 < n) neighbour = std::min(neighbour, pair_gaps[j]);
        f[47] = static_cast<float>((g - neighbour) / t);
        // Columns the lower row fills that already hold text in the run of rows the rules joined
        // ending at the upper one: joining would put two cells' text in one.
        ColumnSet taken;
        for (int k = i; k >= 0 && ufind(k) == ufind(i); --k) taken.insert(cols[k].begin(), cols[k].end());
        const int overlap = shared(cl, taken);
        f[48] = static_cast<float>(cl.empty() ? 0.0 : overlap / static_cast<double>(cl.size()));
        f[49] = static_cast<float>(overlap);
        pair.fixed = split_after.count(i) || split_after.count(j);
        f[50] = split_after.count(i) ? 1.f : 0.f;
        f[51] = split_after.count(j) ? 1.f : 0.f;
        f[52] = header_rows.count(j) ? 1.f : 0.f;
        // The rows around the pair: text centred over a row span sits between fuller rows; a line
        // centred beside wrapped cells has their lines above and below it in columns it leaves empty.
        f[53] = static_cast<float>(i > 0 ? cols[i - 1].size() / nc : -1);
        f[54] = static_cast<float>(j + 1 < n ? cols[j + 1].size() / nc : -1);
        auto around = [&](int above, int middle, int below) {
            int k = 0;
            for (int c : cols[above]) k += cols[below].count(c) && !cols[middle].count(c);
            return static_cast<float>(k / nc);
        };
        f[55] = i > 0 ? around(i - 1, i, j) : 0.f;
        f[56] = j + 1 < n ? around(i, j, j + 1) : 0.f;
        const int given = i < static_cast<int>(control_.given.size()) ? control_.given[i] : -1;
        if (given == 0 || given == 1)
            pair.joined = given == 1;
        else if (control_.use_model && row_model::MODEL_TRAINED)
            pair.joined = row_score(pair.features) >= (pair.rules_join ? -ROW_OVERRIDE_MARGIN : ROW_OVERRIDE_MARGIN);
        else
            pair.joined = pair.rules_join;
        joined[i] = pair.joined;
        if (control_.trace) control_.trace->pairs.append(pair);
    }
    // A row the rules took for text centred over a row span that the decisions put elsewhere is a
    // line of a wrapped cell: its cells no longer span down, nor the header above it across.
    for (int m : split_after)
        if (!joined[m - 1] || joined[m]) {
            for (int s : rows[m]) down.erase(s);
            for (int s : rows[m - 1]) forced.erase(s);
        }
    if (control_.trace)
        for (const auto &r : rows) {
            QVector<QRectF> boxes;
            QStringList texts;
            for (int s : r) {
                boxes.append(QRectF(QPointF(segs_[s].x0, segs_[s].y0), QPointF(segs_[s].x1, segs_[s].y1)));
                texts.append(segs_[s].text());
            }
            control_.trace->rows.append(boxes);
            control_.trace->texts.append(texts);
        }

    // Rows of the table: runs of joined visual rows.
    std::vector<std::vector<int>> groups;
    for (int i = 0; i < n; ++i) {
        if (!groups.empty() && joined[i - 1])
            groups.back().push_back(i);
        else
            groups.push_back({i});
    }
    const int nrows = static_cast<int>(groups.size());
    struct Part {
        int seg, c1;
        bool down;
    };
    std::vector<std::pair<int, int>> part_keys;
    std::map<std::pair<int, int>, std::vector<Part>> parts;
    for (int ri = 0; ri < nrows; ++ri)
        for (int vi : groups[ri])
            for (int p : rows[vi]) {
                const auto it = forced.find(p);
                const auto [c0, c1] = it != forced.end() ? it->second : seg_cols(p);
                const auto key = std::make_pair(ri, c0);
                if (!parts.count(key)) part_keys.push_back(key);
                parts[key].push_back({p, c1, down.count(p) > 0});
            }

    // Cells: union-find over grid positions, the root at the top-left of each merged cell.
    std::vector<int> parent(static_cast<size_t>(nrows) * ncols);
    std::iota(parent.begin(), parent.end(), 0);
    std::vector<uint8_t> has_part(parent.size(), 0);
    for (const auto &key : part_keys) has_part[static_cast<size_t>(key.first) * ncols + key.second] = 1;
    auto find = [&](int k) {
        while (parent[k] != k) {
            parent[k] = parent[parent[k]];
            k = parent[k];
        }
        return k;
    };
    auto join = [&](int r0, int c0, int r1, int c1, bool only_if_empty) {
        const int a = find(r0 * ncols + c0), b = find(r1 * ncols + c1);
        if (a == b) return;
        if (only_if_empty && has_part[a] && has_part[b]) return;
        parent[std::max(a, b)] = std::min(a, b);
        has_part[std::min(a, b)] = has_part[a] || has_part[b];
    };
    for (const auto &key : part_keys)
        for (const auto &p : parts[key]) {
            const auto [ri, c0] = key;
            for (int c = c0 + 1; c <= p.c1; ++c) join(ri, c0, ri, c, false);
            if (p.down && ri + 1 < nrows)
                for (int c = c0; c <= p.c1; ++c) join(ri, c, ri + 1, c, true);
        }
    if (!lattice_ && !rule_mode) {
        // Without rules, a label in the first column with empty cells above and below in equal
        // number is centred over the rows it spans. Odd spans centre on a row; even ones were
        // attached to the upper middle row.
        std::vector<int> filled;
        for (int r = 0; r < nrows; ++r)
            if (parts.count({r, 0})) filled.push_back(r);
        int claimed = filled.empty() ? 0 : filled.front();
        for (size_t k = 1; k < filled.size(); ++k) {
            const int r = filled[k];
            const int next = k + 1 < filled.size() ? filled[k + 1] : nrows;
            const int above = r - claimed - 1, below = next - r - 1;
            claimed = r;
            if (above >= 1 && above <= below && below <= above + 1) {
                for (int rr = r - above; rr < r + below; ++rr) join(rr, 0, rr + 1, 0, true);
                claimed = r + below;
            }
        }
    }
    std::vector<double> ytop, ybot;
    for (const auto &g : groups) {
        double t = 1e18, b = -1e18;
        for (int vi : g) {
            t = std::min(t, top(rows[vi]));
            b = std::max(b, bottom(rows[vi]));
        }
        ytop.push_back(t);
        ybot.push_back(b);
    }
    if (lattice_)
        for (int ri = 0; ri < nrows; ++ri)
            for (int j = 0; j < static_cast<int>(bounds_.size()); ++j)
                if (!vrule_at(bounds_[j], ytop[ri] + 0.2 * text_h_, ybot[ri] - 0.2 * text_h_)) join(ri, j, ri, j + 1, true);
    if (rule_mode) {
        std::vector<double> edges{tx0};
        edges.insert(edges.end(), bounds_.begin(), bounds_.end());
        edges.push_back(tx1);
        for (int ri = 0; ri + 1 < nrows; ++ri) {
            std::vector<bool> ruled_cols;
            for (int c = 0; c < ncols; ++c)
                ruled_cols.push_back(hrule_between(ybot[ri], ytop[ri + 1], edges[c], edges[c + 1], 0.5 * (edges[c + 1] - edges[c])));
            // A missing rule means a span only where the boundary is ruled elsewhere.
            if (std::none_of(ruled_cols.begin(), ruled_cols.end(), [](bool b) { return b; })) continue;
            for (int c = 0; c < ncols; ++c)
                if (!ruled_cols[c]) join(ri, c, ri + 1, c, true);
        }
    }

    // Cells in reading order, then rows and columns without text dropped.
    std::map<int, std::vector<int>> members;
    for (int k = 0; k < nrows * ncols; ++k) members[find(k)].push_back(k);
    std::vector<Cell> out;
    auto boxes_of = [&](const std::vector<int> &segments) {
        QVector<QRectF> boxes;
        for (int s : segments) boxes.append(QRectF(QPointF(segs_[s].x0, segs_[s].y0), QPointF(segs_[s].x1, segs_[s].y1)));
        return boxes;
    };
    for (const auto &[root, mem] : members) {
        int r1 = 0, c1 = 0;
        std::vector<int> segments;
        for (int k : mem) {
            r1 = std::max(r1, k / ncols);
            c1 = std::max(c1, k % ncols);
            const auto it = parts.find({k / ncols, k % ncols});
            if (it != parts.end())
                for (const auto &p : it->second) segments.push_back(p.seg);
        }
        const int r0 = root / ncols, c0 = root % ncols;
        out.push_back({r0, c0, r1 - r0 + 1, c1 - c0 + 1, join_text(segments), boxes_of(segments)});
    }
    std::set<int> used_r, used_c;
    for (const auto &c : out)
        if (!c.text.isEmpty()) {
            used_r.insert(c.row);
            used_c.insert(c.column);
        }
    const std::vector<int> rlist(used_r.begin(), used_r.end()), clist(used_c.begin(), used_c.end());
    auto index_of = [](const std::vector<int> &list, int v) { return static_cast<int>(std::lower_bound(list.begin(), list.end(), v) - list.begin()); };
    auto last_before = [](const std::vector<int> &list, int limit) {
        int v = -1;
        for (int x : list)
            if (x < limit) v = x;
        return v;
    };
    Table table;
    table.rows = static_cast<int>(rlist.size());
    table.columns = static_cast<int>(clist.size());
    for (const auto &c : out) {
        if (!used_r.count(c.row) || !used_c.count(c.column)) continue;
        const int r = index_of(rlist, c.row), col = index_of(clist, c.column);
        const int r_end = index_of(rlist, last_before(rlist, c.row + c.row_span));
        const int c_end = index_of(clist, last_before(clist, c.column + c.column_span));
        table.cells.push_back({r, col, r_end - r + 1, c_end - col + 1, c.text, c.boxes});
    }
    // The title above and the note below, each one cell across every column.
    auto across = [&](const std::vector<Row> &lines, int row) {
        std::vector<int> parts;
        for (const auto &r : lines) parts.insert(parts.end(), r.begin(), r.end());
        return Cell{row, 0, 1, table.columns, join_text(parts), boxes_of(parts)};
    };
    if (table.columns > 0 && !title_.empty()) {
        for (auto &c : table.cells) ++c.row;
        table.cells.push_back(across(title_, 0));
        ++table.rows;
        table.title_rows = 1;
    }
    if (table.columns > 0 && !note_.empty()) {
        table.cells.push_back(across(note_, table.rows));
        ++table.rows;
        table.note_rows = 1;
    }
    std::sort(table.cells.begin(), table.cells.end(), [](const Cell &a, const Cell &b) { return std::tie(a.row, a.column) < std::tie(b.row, b.column); });
    return table;
}
}

bool Table::operator==(const Table &other) const {
    if (rows != other.rows || columns != other.columns || cells.size() != other.cells.size()) return false;
    for (int k = 0; k < cells.size(); ++k) {
        const auto &a = cells[k], &b = other.cells[k];
        if (a.row != b.row || a.column != b.column || a.row_span != b.row_span || a.column_span != b.column_span || a.text != b.text) return false;
    }
    return true;
}

QVector<Line> table_lines(const QVector<OcrLine> &lines) {
    QVector<Line> out;
    for (const auto &source : lines) {
        if (source.text.trimmed().isEmpty() || source.box.size() < 4) continue;
        const QRectF rect = QPolygonF(source.box).boundingRect();
        if (!source.words.isEmpty()) {
            Line line{source.text, rect.left(), rect.right(), rect.top(), rect.bottom(), {}};
            for (const auto &word : source.words) {
                if (word.box.size() < 4) continue;
                const QRectF w = QPolygonF(word.box).boundingRect();
                line.words.append({word.text, w.left(), w.right(), w.top(), w.bottom()});
            }
            if (!line.words.isEmpty()) out.append(line);
            continue;
        }
        // Without word boxes a vertical line, or one without a usable frame, is one word.
        const auto frame = line_frame(source.box, false);
        if (source.vertical || !frame.valid()) {
            out.append(Line{source.text, rect.left(), rect.right(), rect.top(), rect.bottom(), {{source.text.trimmed(), rect.left(), rect.right(), rect.top(), rect.bottom()}}});
            continue;
        }
        const auto points = source.text.toUcs4();
        const auto spans = source.chars.size() == points.size() ? source.chars : estimate_char_spans(source.text);
        if (spans.size() != points.size()) continue;
        const auto to_source = frame.to_source();
        auto x_at = [&](double fraction) { return to_source.map(QPointF(fraction * frame.length, frame.thickness / 2)).x(); };
        Line line{source.text, rect.left(), rect.right(), rect.top(), rect.bottom(), {}};
        std::optional<Word> current;
        auto flush = [&] {
            if (current) line.words.append(*current);
            current.reset();
        };
        for (int i = 0; i < points.size(); ++i) {
            const char32_t c = points[i];
            const QString ch = QString::fromUcs4(&c, 1);
            if (QChar::isSpace(c)) {
                flush();
                continue;
            }
            const double a = x_at(spans[i].start), b = x_at(spans[i].end);
            // CJK characters and Hangul syllables are words of their own, as the engine reports them.
            const bool alone = c < 0x10000 && (is_cjk(QChar(static_cast<ushort>(c))) || (c >= 0xAC00 && c <= 0xD7A3));
            if (alone) {
                flush();
                line.words.append({ch, std::min(a, b), std::max(a, b), rect.top(), rect.bottom()});
            } else if (current) {
                current->text += ch;
                current->x1 = std::max(current->x1, std::max(a, b));
            } else {
                current = Word{ch, std::min(a, b), std::max(a, b), rect.top(), rect.bottom()};
            }
        }
        flush();
        if (!line.words.isEmpty()) out.append(line);
    }
    return out;
}

const std::array<const char *, CAPTION_FEATURE_COUNT> CAPTION_FEATURE_NAMES = {
    "above", "distance", "gap_toward", "gap_to_table", "segs", "width", "left_inset", "right_inset", "off_centre", "h_median",
    "h_max", "h_vs_body", "chars", "digits", "cjk", "colon_end", "stop_end", "note_mark", "caption_mark", "runs", "longest_run",
    "straddle_seg", "rule_between", "fill_diff", "from_image_edge", "edge_space", "extent_score", "rules_in", "rows_outside",
    "table_rows"};

const std::array<const char *, EXTENT_FEATURE_COUNT> EXTENT_FEATURE_NAMES = {
    "from_top", "from_bottom", "visual_rows", "segs", "width", "left_inset", "right_inset", "off_centre", "h_median", "h_max",
    "gap_above", "gap_below", "gap_above_med", "gap_below_med", "straddle_seg", "straddle_row", "bounds", "cols", "longest_run",
    "runs", "rule_above", "rule_below", "in_frame", "above_frame", "below_frame", "vrule_through", "fill_diff_below",
    "fill_diff_above", "rules_in", "colon_end", "stop_end", "note_mark", "caption_mark", "chars", "digits", "cjk",
    "segs_above", "segs_below", "shared_below", "shared_above", "fill", "segs_vs_typical"};

const std::array<const char *, ROW_FEATURE_COUNT> ROW_FEATURE_NAMES = {
    "gap", "gap_prev", "gap_next", "gap_med", "gap_row", "col_gap_min", "col_gap_max", "h_upper", "h_lower",
    "cols_upper", "cols_lower", "cols_shared", "lower_within", "upper_within", "cols_new", "cols_left", "segs_upper",
    "segs_lower", "columns", "ruled", "long_rule", "rule_mode", "lattice", "same_band", "band_rows", "continues",
    "upper_wide", "left_aligned", "index", "position", "head_zone", "rules_join", "header_over", "cell_lines", "cjk",
    "upper_open_end", "lower_lowercase", "lower_bracket", "upper_numeric", "lower_numeric", "fill_diff", "visual_rows",
    "centred", "gap_rank", "gap_p80", "gap_split", "split_spread", "gap_local", "taken_share", "taken_count",
    "centred_upper", "centred_lower", "header_below", "cols_before", "cols_after", "around_upper", "around_lower"};

Table build_table(const QImage &image, const QVector<Line> &lines) {
    static const RowControl usual;
    return build_table(image, lines, usual);
}

Table build_table(const QImage &image, const QVector<Line> &lines, const RowControl &control) {
    if (lines.isEmpty()) return {};
    return Builder(image, lines, control).build();
}

bool is_table(const Table &table) {
    // The title and note rows say nothing about whether the rest is a table.
    const int body = table.rows - table.title_rows - table.note_rows;
    if (body < 2 || table.columns < 2) return false;
    std::vector<int> filled(table.rows, 0);
    for (const auto &c : table.cells)
        if (!c.text.isEmpty()) ++filled[c.row];
    const auto multi = std::count_if(filled.begin() + table.title_rows, filled.end() - table.note_rows, [](int k) { return k >= 2; });
    return multi >= 2 && multi * 2 >= body;
}

QString table_html(const Table &table) {
    QString html = "<table border=\"1\" cellspacing=\"0\" cellpadding=\"4\" style=\"border-collapse:collapse\">";
    int k = 0;
    for (int r = 0; r < table.rows; ++r) {
        html += "<tr>";
        for (; k < table.cells.size() && table.cells[k].row == r; ++k) {
            const auto &c = table.cells[k];
            html += "<td";
            if (c.row_span > 1) html += QString(" rowspan=\"%1\"").arg(c.row_span);
            if (c.column_span > 1) html += QString(" colspan=\"%1\"").arg(c.column_span);
            html += ">" + c.text.toHtmlEscaped().replace('\n', "<br>") + "</td>";
        }
        html += "</tr>";
    }
    return html + "</table>";
}

QString table_tsv(const Table &table) {
    QVector<QStringList> grid(table.rows, QStringList(table.columns, QString()));
    for (const auto &c : table.cells) {
        QString text = c.text;
        text.replace('\t', ' ').replace('\n', ' ').replace('\r', ' ');
        grid[c.row][c.column] = text;
    }
    QStringList lines;
    for (const auto &row : grid) lines.append(row.join('\t'));
    return lines.join('\n');
}
}

namespace jietuba::text_table {
namespace {
void sort_cells(Table &table) {
    std::sort(table.cells.begin(), table.cells.end(), [](const Cell &a, const Cell &b) { return std::tie(a.row, a.column) < std::tie(b.row, b.column); });
}
}

const Cell *cell_at(const Table &table, int row, int column) {
    for (const auto &c : table.cells)
        if (c.row <= row && row < c.row + c.row_span && c.column <= column && column < c.column + c.column_span) return &c;
    return nullptr;
}

Table with_text(Table table, int row, int column, const QString &text) {
    for (auto &c : table.cells)
        if (c.row <= row && row < c.row + c.row_span && c.column <= column && column < c.column + c.column_span && c.text != text) {
            c.text = text;
            c.boxes.clear();
        }
    return table;
}

Table without_row(const Table &table, int row) {
    if (row < 0 || row >= table.rows) return table;
    Table out{table.rows - 1, table.columns, {}};
    for (auto c : table.cells) {
        if (c.row <= row && row < c.row + c.row_span) {
            if (c.row_span == 1) continue;
            --c.row_span;
        } else if (c.row > row) {
            --c.row;
        }
        out.cells.append(c);
    }
    sort_cells(out);
    return out;
}

Table without_column(const Table &table, int column) {
    if (column < 0 || column >= table.columns) return table;
    Table out{table.rows, table.columns - 1, {}};
    for (auto c : table.cells) {
        if (c.column <= column && column < c.column + c.column_span) {
            if (c.column_span == 1) continue;
            --c.column_span;
        } else if (c.column > column) {
            --c.column;
        }
        out.cells.append(c);
    }
    sort_cells(out);
    return out;
}

Table with_row_inserted(const Table &table, int row) {
    row = std::clamp(row, 0, table.rows);
    Table out{table.rows + 1, table.columns, {}};
    QVector<bool> covered(table.columns, false);
    for (auto c : table.cells) {
        if (c.row < row && row < c.row + c.row_span) {
            ++c.row_span;
            for (int k = c.column; k < c.column + c.column_span; ++k) covered[k] = true;
        } else if (c.row >= row) {
            ++c.row;
        }
        out.cells.append(c);
    }
    for (int k = 0; k < table.columns; ++k)
        if (!covered[k]) out.cells.append({row, k, 1, 1, {}});
    sort_cells(out);
    return out;
}

Table with_column_inserted(const Table &table, int column) {
    column = std::clamp(column, 0, table.columns);
    Table out{table.rows, table.columns + 1, {}};
    QVector<bool> covered(table.rows, false);
    for (auto c : table.cells) {
        if (c.column < column && column < c.column + c.column_span) {
            ++c.column_span;
            for (int r = c.row; r < c.row + c.row_span; ++r) covered[r] = true;
        } else if (c.column >= column) {
            ++c.column;
        }
        out.cells.append(c);
    }
    for (int r = 0; r < table.rows; ++r)
        if (!covered[r]) out.cells.append({r, column, 1, 1, {}});
    sort_cells(out);
    return out;
}

Table part(const Table &table, int row0, int column0, int row1, int column1) {
    row0 = std::max(row0, 0);
    column0 = std::max(column0, 0);
    row1 = std::min(row1, table.rows - 1);
    column1 = std::min(column1, table.columns - 1);
    if (row1 < row0 || column1 < column0) return {};
    Table out{row1 - row0 + 1, column1 - column0 + 1, {}};
    for (const auto &c : table.cells) {
        const int r0 = std::max(c.row, row0), r1 = std::min(c.row + c.row_span - 1, row1);
        const int k0 = std::max(c.column, column0), k1 = std::min(c.column + c.column_span - 1, column1);
        if (r1 < r0 || k1 < k0) continue;
        out.cells.append({r0 - row0, k0 - column0, r1 - r0 + 1, k1 - k0 + 1, c.text, c.boxes});
    }
    sort_cells(out);
    return out;
}

bool can_merge(const Table &table, int row0, int column0, int row1, int column1) {
    if (row0 < 0 || column0 < 0 || row1 >= table.rows || column1 >= table.columns || row1 < row0 || column1 < column0) return false;
    int inside = 0;
    for (const auto &c : table.cells) {
        const bool meets = c.row <= row1 && row0 < c.row + c.row_span && c.column <= column1 && column0 < c.column + c.column_span;
        if (!meets) continue;
        if (c.row < row0 || c.row + c.row_span - 1 > row1 || c.column < column0 || c.column + c.column_span - 1 > column1) return false;
        ++inside;
    }
    return inside >= 2;
}

Table merged(const Table &table, int row0, int column0, int row1, int column1) {
    if (!can_merge(table, row0, column0, row1, column1)) return table;
    Table out{table.rows, table.columns, {}};
    QString text;
    QVector<QRectF> boxes;
    for (const auto &c : table.cells) {
        if (c.row < row0 || c.row > row1 || c.column < column0 || c.column > column1) {
            out.cells.append(c);
            continue;
        }
        boxes += c.boxes;
        const QString t = c.text.trimmed();
        if (t.isEmpty()) continue;
        if (!text.isEmpty() && !(ends_cjk(text) || starts_cjk(t))) text += ' ';
        text += t;
    }
    out.cells.append({row0, column0, row1 - row0 + 1, column1 - column0 + 1, text, boxes});
    sort_cells(out);
    return out;
}

Table split(const Table &table, int row0, int column0, int row1, int column1) {
    Table out{table.rows, table.columns, {}};
    for (const auto &c : table.cells) {
        const bool meets = c.row <= row1 && row0 < c.row + c.row_span && c.column <= column1 && column0 < c.column + c.column_span;
        if (!meets || (c.row_span == 1 && c.column_span == 1)) {
            out.cells.append(c);
            continue;
        }
        for (int r = c.row; r < c.row + c.row_span; ++r)
            for (int k = c.column; k < c.column + c.column_span; ++k) {
                const bool top_left = r == c.row && k == c.column;
                out.cells.append({r, k, 1, 1, top_left ? c.text : QString(), top_left ? c.boxes : QVector<QRectF>()});
            }
    }
    sort_cells(out);
    return out;
}

QVector<QStringList> parse_tsv(const QString &text) {
    QVector<QStringList> rows;
    QStringList row;
    QString field;
    bool quoted = false, any = false;
    for (int i = 0; i < text.size(); ++i) {
        const QChar ch = text[i];
        if (quoted) {
            if (ch != '"') field += ch;
            else if (i + 1 < text.size() && text[i + 1] == '"') field += '"', ++i;
            else quoted = false;
        } else if (ch == '"' && field.isEmpty()) {
            quoted = true;
        } else if (ch == '\t') {
            row << field;
            field.clear();
        } else if (ch == '\n' || ch == '\r') {
            if (ch == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
            rows << (row << field);
            row.clear();
            field.clear();
            any = false;
            continue;
        } else {
            field += ch;
        }
        any = true;
    }
    // A trailing line break, as spreadsheets end their copies, adds no empty row.
    if (any || !row.isEmpty()) rows << (row << field);
    return rows;
}

Table pasted(Table table, int row, int column, const QVector<QStringList> &grid) {
    if (row < 0 || column < 0) return table;
    int width = 0;
    for (const auto &line : grid) width = std::max(width, int(line.size()));
    while (table.rows < row + grid.size()) table = with_row_inserted(table, table.rows);
    while (table.columns < column + width) table = with_column_inserted(table, table.columns);
    for (auto &c : table.cells) {
        const int r = c.row - row, k = c.column - column;
        if (r >= 0 && r < grid.size() && k >= 0 && k < grid[r].size() && c.text != grid[r][k]) {
            c.text = grid[r][k];
            c.boxes.clear();
        }
    }
    return table;
}

Table map_text(Table table, const std::function<QString(const QString &)> &convert) {
    for (auto &c : table.cells) c.text = convert(c.text);
    return table;
}

QString column_name(int column) {
    QString name;
    for (int n = column + 1; n > 0; n = (n - 1) / 26) name.prepend(QChar('A' + (n - 1) % 26));
    return name;
}

std::optional<Number> parse_number(const QString &text) {
    QString s = text.normalized(QString::NormalizationForm_KC).trimmed();
    s.replace(QChar(0x2212), '-');
    bool negative = false;
    if (s.size() > 2 && s.front() == '(' && s.back() == ')') {
        negative = true;
        s = s.mid(1, s.size() - 2).trimmed();
    }
    Number number;
    if (s.endsWith('%')) {
        number.percent = true;
        s = s.chopped(1).trimmed();
    }
    const auto take_sign = [&] {
        if (!s.startsWith('-') && !s.startsWith('+')) return false;
        if (s.front() == '-') negative = !negative;
        s = s.mid(1).trimmed();
        return true;
    };
    // The sign may come before or after the currency: -$5 and $-5.
    const bool signed_first = take_sign();
    if (!s.isEmpty() && QStringLiteral("$¥€£").contains(s.front())) {
        s = s.mid(1).trimmed();
        if (!signed_first) take_sign();
    }
    static const QRegularExpression pattern(R"(^([0-9]{1,3}(?:,[0-9]{3})+|[0-9]+)(?:\.([0-9]+))?$)");
    const auto match = pattern.match(s);
    if (!match.hasMatch()) return std::nullopt;
    const QString fraction = match.captured(2);
    bool ok = false;
    number.value = (match.captured(1).remove(',') + (fraction.isEmpty() ? QString() : "." + fraction)).toDouble(&ok);
    if (!ok) return std::nullopt;
    number.decimals = int(fraction.size());
    if (negative) number.value = -number.value;
    return number;
}

Summary summarize(const QStringList &texts) {
    Summary summary;
    summary.percent = true;
    for (const auto &text : texts) {
        if (text.trimmed().isEmpty()) continue;
        ++summary.count;
        const auto number = parse_number(text);
        if (!number) continue;
        ++summary.numbers;
        summary.sum += number->value;
        summary.decimals = std::max(summary.decimals, number->decimals);
        summary.percent = summary.percent && number->percent;
    }
    summary.percent = summary.percent && summary.numbers > 0;
    return summary;
}

QString format_number(double value, int decimals) {
    QString digits = QString::number(std::abs(value), 'f', decimals);
    const auto point = digits.indexOf('.');
    for (auto k = (point < 0 ? digits.size() : point) - 3; k > 0; k -= 3) digits.insert(k, ',');
    // No minus on a value that rounds to zero.
    const bool zero = std::all_of(digits.begin(), digits.end(), [](QChar ch) { return !ch.isDigit() || ch == '0'; });
    return value < 0 && !zero ? "-" + digits : digits;
}
}
