#include "text_recognition/table.h"
#include "ocr/ocr_line_geometry.h"
#include <QPolygonF>
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
    Builder(const QImage &image, const QVector<Line> &lines) : image_(image) { make_segments(lines); }
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
    QString join_text(std::vector<int> parts) const;

    QImage image_;
    std::vector<Seg> segs_;
    double text_h_ = 0;
    std::vector<Rule> hrules_, vrules_;
    std::vector<double> bounds_;
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
    const auto gray = gray_of(image_);
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
    auto rows = drop_context(visual_rows());

    // Prose lines have gaps like merged cells, so the first pass counts only rows of several segments.
    find_columns(rows, false);
    // Prose above or below the table runs through column boundaries; its short last line goes with it.
    auto straddles = [&](const Row &row) {
        int n = 0;
        for (double b : bounds_) {
            bool hit = false;
            for (int s : row)
                for (const auto &w : segs_[s].words)
                    if (w.x0 + 1 < b && b < w.x1 - 1) hit = true;
            if (hit) ++n;
        }
        return n;
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
        // A header above the row-spanning cells covers the columns those cells leave free.
        auto free = [&](int c) { return c >= 0 && c < ncols && !cols[i].count(c) && cols[i + 1].count(c) && !cols[i - 1].count(c); };
        for (int u : rows[i - 1]) {
            auto [c0, c1] = seg_cols(u);
            while (free(c0 - 1)) --c0;
            while (free(c1 + 1)) ++c1;
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

    // Rows of the table: runs of united visual rows.
    std::vector<std::vector<int>> groups;
    for (int i = 0; i < n; ++i) {
        if (!groups.empty() && ufind(i) == ufind(i - 1))
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
        out.push_back({r0, c0, r1 - r0 + 1, c1 - c0 + 1, join_text(segments)});
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
        table.cells.push_back({r, col, r_end - r + 1, c_end - col + 1, c.text});
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

Table build_table(const QImage &image, const QVector<Line> &lines) {
    if (lines.isEmpty()) return {};
    return Builder(image, lines).build();
}

bool is_table(const Table &table) {
    if (table.rows < 2 || table.columns < 2) return false;
    std::vector<int> filled(table.rows, 0);
    for (const auto &c : table.cells)
        if (!c.text.isEmpty()) ++filled[c.row];
    const auto multi = std::count_if(filled.begin(), filled.end(), [](int k) { return k >= 2; });
    return multi >= 2 && multi * 2 >= table.rows;
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
        if (c.row <= row && row < c.row + c.row_span && c.column <= column && column < c.column + c.column_span) c.text = text;
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
        out.cells.append({r0 - row0, k0 - column0, r1 - r0 + 1, k1 - k0 + 1, c.text});
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
    for (const auto &c : table.cells) {
        if (c.row < row0 || c.row > row1 || c.column < column0 || c.column > column1) {
            out.cells.append(c);
            continue;
        }
        const QString t = c.text.trimmed();
        if (t.isEmpty()) continue;
        if (!text.isEmpty() && !(ends_cjk(text) || starts_cjk(t))) text += ' ';
        text += t;
    }
    out.cells.append({row0, column0, row1 - row0 + 1, column1 - column0 + 1, text});
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
            for (int k = c.column; k < c.column + c.column_span; ++k) out.cells.append({r, k, 1, 1, r == c.row && k == c.column ? c.text : QString()});
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
        if (r >= 0 && r < grid.size() && k >= 0 && k < grid[r].size()) c.text = grid[r][k];
    }
    return table;
}

Table map_text(Table table, const std::function<QString(const QString &)> &convert) {
    for (auto &c : table.cells) c.text = convert(c.text);
    return table;
}
}
