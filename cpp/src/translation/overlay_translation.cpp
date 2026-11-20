#include "translation/overlay_translation.h"
#include "ocr/ocr_line_geometry.h"
#include "translation/line_grouping.h"
#include <QFont>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QPainter>
#include <QPolygonF>
#include <QStringList>
#include <QTextBoundaryFinder>
#include <QTextLayout>
#include <QTextOption>
#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>
#include <vector>

namespace jietuba {
namespace {
// A translation is set no smaller than this, nor than its source when that is smaller still;
// one that does not fit even so leaves the original on the image and is shown as text.
constexpr int MIN_OVERLAY_FONT = 8;
constexpr int MAX_OVERLAY_FONT = 96;
// The size texts are measured at before being scaled to their boxes.
constexpr int PROBE_FONT = 64;
// The largest font size a line box of height 1 can hold: a box hugs at least the ink from
// ascender to descender, which is nearly the size itself.
constexpr double SIZE_PER_BOX_HEIGHT = 1.05;

// The page colour behind a block is read along rows of pixels across it, at most this many
// rows and this many samples a row.
constexpr int SCAN_ROWS = 9;
constexpr int MAX_ROW_SAMPLES = 480;
// A sample this close per channel to the one before it continues a run: a gradient changes a
// little at a time, the edge of a glyph all at once.
constexpr int RUN_TOLERANCE = 10;
// A run at least this long is page: strokes and their antialiased edges are shorter.
constexpr int MIN_PAGE_RUN = 4;
// Runs this close per channel are one colour.
constexpr int SAME_RUN_COLOR = 14;
// A pixel this close per channel to the page colour is plain page.
constexpr int PLAIN_TOLERANCE = 24;
// A pixel this close per channel to a blend of the page and the ink colour is ink.
constexpr int INK_BLEND_TOLERANCE = 40;

int channel_distance(QRgb a, QRgb b) {
    return std::max({std::abs(qRed(a) - qRed(b)), std::abs(qGreen(a) - qGreen(b)), std::abs(qBlue(a) - qBlue(b))});
}

QColor median_color(const std::vector<QRgb> &colors) {
    if (colors.empty()) return {};
    const auto middle = colors.size() / 2;
    std::vector<int> red, green, blue;
    red.reserve(colors.size());
    green.reserve(colors.size());
    blue.reserve(colors.size());
    for (const auto color : colors) {
        red.push_back(qRed(color));
        green.push_back(qGreen(color));
        blue.push_back(qBlue(color));
    }
    std::nth_element(red.begin(), red.begin() + middle, red.end());
    std::nth_element(green.begin(), green.begin() + middle, green.end());
    std::nth_element(blue.begin(), blue.begin() + middle, blue.end());
    return QColor(red[middle], green[middle], blue[middle]);
}

struct PixelSource {
    const QImage &image;
    QTransform to_image{};
    QRegion excluded{};

    std::optional<QRgb> pixel(int x, int y) const {
        const QPointF point = to_image.map(QPointF(x + 0.5, y + 0.5));
        if (!(point.x() >= 0 && point.x() < image.width() && point.y() >= 0 && point.y() < image.height())) return std::nullopt;
        const QPoint cell(static_cast<int>(point.x()), static_cast<int>(point.y()));
        if (excluded.contains(cell)) return std::nullopt;
        return image.pixel(cell);
    }
};

// A run of one colour along a row of samples: the colour of its middle sample, a pixel that
// is really there, and its length in samples.
struct Run {
    QRgb color = 0;
    int length = 0;
};

// The runs along row y from x0 to x1, a sample every step pixels, into runs and every sample
// read into seen. A sample off the image ends a run; runs shorter than MIN_PAGE_RUN are dropped.
void collect_runs(std::vector<Run> &runs, std::vector<QRgb> &seen, const PixelSource &source, int y, int x0, int x1, int step) {
    int start = x0, length = 0;
    QRgb previous = 0;
    const auto close = [&] {
        if (length >= MIN_PAGE_RUN) runs.push_back({*source.pixel(start + (length / 2) * step, y), length});
        length = 0;
    };
    for (int x = x0; x <= x1; x += step) {
        const auto color = source.pixel(x, y);
        if (!color) {
            close();
            continue;
        }
        seen.push_back(*color);
        if (length > 0 && channel_distance(*color, previous) <= RUN_TOLERANCE) {
            ++length;
            previous = *color;
            continue;
        }
        close();
        start = x;
        previous = *color;
        length = 1;
    }
    close();
}

// The colours the runs add up to, each with the colour of its longest run, most length first.
std::vector<Run> run_colors(std::vector<Run> runs) {
    std::stable_sort(runs.begin(), runs.end(), [](const Run &a, const Run &b) { return a.length > b.length; });
    std::vector<Run> colors;
    for (const auto &run : runs) {
        const auto same = std::find_if(colors.begin(), colors.end(), [&run](const Run &color) { return channel_distance(color.color, run.color) <= SAME_RUN_COLOR; });
        if (same == colors.end()) colors.push_back(run);
        else same->length += run.length;
    }
    std::stable_sort(colors.begin(), colors.end(), [](const Run &a, const Run &b) { return a.length > b.length; });
    return colors;
}

// The colour of the page behind a block: the colour that runs longest along rows across it,
// since glyphs break the page into shorter runs. Bold glyphs can run about as long as the page
// between them, so when the second colour has at least two fifths of the first's length, the
// page is the one more of the line just around the box shows, where ink seldom reaches. Other
// text on that line is skipped. A block on a busy picture, with no run anywhere, takes the
// median of what it shows.
QColor sample_background(const PixelSource &source, const QRect &block, const QVector<QRect> &other_text = {}) {
    if (block.isEmpty()) return {};
    std::vector<Run> runs;
    std::vector<QRgb> seen;
    const int step = std::max(1, (block.width() + MAX_ROW_SAMPLES - 1) / MAX_ROW_SAMPLES);
    const int rows = std::min(SCAN_ROWS, block.height());
    for (int row = 0; row < rows; ++row)
        collect_runs(runs, seen, source, block.top() + (2 * row + 1) * block.height() / (2 * rows), block.left(), block.right(), step);
    const auto colors = run_colors(runs);
    if (colors.empty()) return seen.empty() ? QColor(Qt::white) : median_color(seen);
    if (colors.size() > 1 && colors[1].length * 5 >= colors[0].length * 2) {
        const auto around = [&](QRgb color) {
            int support = 0;
            const auto count = [&](int x, int y) {
                if (std::any_of(other_text.cbegin(), other_text.cend(), [x, y](const QRect &rect) { return rect.contains(x, y); })) return;
                const auto pixel = source.pixel(x, y);
                if (pixel && channel_distance(*pixel, color) <= PLAIN_TOLERANCE) ++support;
            };
            for (int x = block.left() - 1; x <= block.right() + 1; ++x) {
                count(x, block.top() - 1);
                count(x, block.bottom() + 1);
            }
            for (int y = block.top(); y <= block.bottom(); ++y) {
                count(block.left() - 1, y);
                count(block.right() + 1, y);
            }
            return support;
        };
        if (around(colors[1].color) > around(colors[0].color)) return QColor(colors[1].color);
    }
    return QColor(colors[0].color);
}

// The colour the original text was drawn in: the median of the pixels standing out most from
// the background, which are the glyph cores rather than their antialiased edges. A textless
// block keeps a plain black or white, whichever the background asks for.
QColor sample_foreground(const PixelSource &source, const QRect &bounds, const QColor &background) {
    std::vector<std::pair<QRgb, int>> candidates;
    if (!bounds.isEmpty()) {
        const int step = std::max(1, static_cast<int>(std::sqrt(bounds.width() * double(bounds.height()) / 1500.0)));
        for (int y = bounds.top(); y < bounds.bottom(); y += step) {
            for (int x = bounds.left(); x < bounds.right(); x += step) {
                const auto color = source.pixel(x, y);
                if (!color) continue;
                const int contrast = std::abs(qRed(*color) - background.red()) + std::abs(qGreen(*color) - background.green()) +
                                     std::abs(qBlue(*color) - background.blue());
                if (contrast > 180) candidates.emplace_back(*color, contrast);
            }
        }
    }
    if (candidates.empty()) return background.lightness() < 128 ? QColor(255, 255, 255) : QColor(32, 32, 32);
    int strongest = 0;
    for (const auto &[color, contrast] : candidates) strongest = std::max(strongest, contrast);
    std::vector<QRgb> ink;
    ink.reserve(candidates.size());
    for (const auto &[color, contrast] : candidates)
        if (contrast * 4 >= strongest * 3) ink.push_back(color);
    return median_color(ink);
}

// How far past its recognized box a block's original ink reaches: a box can clip an
// antialiased edge, an accent or a descender. Two pixels past the box are always covered; each
// side then moves out a pixel at a time, at most most pixels, while the off-page pixels on the
// line beyond it are mostly the text's ink: its colour or a blend of it with the page. A clean
// line ends it, and so does one nearly all off the page, or off it in another colour: a frame,
// a rule or the edge of a picture, none of which are the text's.
QRect ink_cover(const QImage &source, const QRect &box, const QColor &page, const QColor &ink, const QVector<QRect> &other_text, int most) {
    QRect cover = box.adjusted(-2, -2, 2, 2).intersected(source.rect());
    if (cover.isEmpty()) return cover;
    const QRgb plain = page.rgb();
    const double toward[3] = {double(ink.red() - page.red()), double(ink.green() - page.green()), double(ink.blue() - page.blue())};
    const double reach = toward[0] * toward[0] + toward[1] * toward[1] + toward[2] * toward[2];
    // Within INK_BLEND_TOLERANCE per channel of the page-to-ink blend nearest to it.
    const auto inky = [&](QRgb color) {
        const double offset[3] = {double(qRed(color) - page.red()), double(qGreen(color) - page.green()), double(qBlue(color) - page.blue())};
        const double share = reach > 0 ? std::clamp((offset[0] * toward[0] + offset[1] * toward[1] + offset[2] * toward[2]) / reach, 0.0, 1.0) : 0.0;
        for (int channel = 0; channel < 3; ++channel)
            if (std::abs(offset[channel] - share * toward[channel]) > INK_BLEND_TOLERANCE) return false;
        return true;
    };
    const auto keeps_growing = [&](const QPoint &from, const QPoint &step, int count) {
        int off = 0, looked = 0, ink_like = 0;
        for (int index = 0; index < count; ++index) {
            const QPoint point = from + step * index;
            if (std::any_of(other_text.cbegin(), other_text.cend(), [&point](const QRect &rect) { return rect.contains(point); })) continue;
            ++looked;
            const QRgb color = source.pixel(point);
            if (channel_distance(color, plain) <= PLAIN_TOLERANCE) continue;
            ++off;
            if (inky(color)) ++ink_like;
        }
        return off > 0 && off * 10 < looked * 9 && ink_like * 2 >= off;
    };
    for (int grown = 0; grown < most && cover.top() > 0 && keeps_growing({cover.left(), cover.top() - 1}, {1, 0}, cover.width()); ++grown)
        cover.setTop(cover.top() - 1);
    for (int grown = 0; grown < most && cover.bottom() < source.height() - 1 && keeps_growing({cover.left(), cover.bottom() + 1}, {1, 0}, cover.width()); ++grown)
        cover.setBottom(cover.bottom() + 1);
    for (int grown = 0; grown < most && cover.left() > 0 && keeps_growing({cover.left() - 1, cover.top()}, {0, 1}, cover.height()); ++grown)
        cover.setLeft(cover.left() - 1);
    for (int grown = 0; grown < most && cover.right() < source.width() - 1 && keeps_growing({cover.right() + 1, cover.top()}, {0, 1}, cover.height()); ++grown)
        cover.setRight(cover.right() + 1);
    return cover;
}

// Han characters take a different form in each CJK language, so each gets its own face; other
// scripts use the UI face and per-glyph fallback. Empty when none is installed.
QString overlay_font_family(const QString &target_lang) {
    const auto lang = target_lang.toUpper();
    QStringList wanted;
    if (lang.startsWith("ZH-HANT") || lang == "ZH-TW" || lang == "ZH-HK") wanted = {"Microsoft JhengHei UI", "Microsoft JhengHei", "PMingLiU"};
    else if (lang.startsWith("ZH")) wanted = {"Microsoft YaHei UI", "Microsoft YaHei", "SimHei"};
    else if (lang.startsWith("JA")) wanted = {"Yu Gothic UI", "Meiryo UI", "Meiryo", "MS UI Gothic"};
    else if (lang.startsWith("KO")) wanted = {"Malgun Gothic", "Gulim"};
    wanted << "Segoe UI" << "Arial";
    for (const auto &family : wanted) {
        if (QFontInfo(QFont(family)).family().compare(family, Qt::CaseInsensitive) == 0) return family;
    }
    return {};
}

QFont sized(const QString &family, int pixels) {
    QFont font(family);
    font.setPixelSize(std::clamp(pixels, 1, MAX_OVERLAY_FONT));
    return font;
}

// The size the original text was set in: the size at which each line's ink fills its own
// box, the middle one of a paragraph's lines. A recognized box hugs its glyphs, so the box
// height alone says less than the ink does. Measured in a plain UI face: the face chosen for
// the target language may be much wider or narrower in the source's script.
double source_font_size(const OverlayTextBlock &block) {
    const auto texts = block.line_texts.isEmpty() ? block.source.split('\n') : block.line_texts;
    const QFontMetricsF metrics(sized(QStringLiteral("Segoe UI"), PROBE_FONT));
    std::vector<double> sizes;
    for (int index = 0; index < texts.size() && index < block.line_rects.size(); ++index) {
        const auto ink = metrics.tightBoundingRect(texts[index]);
        const auto &box = block.line_rects[index];
        if (ink.width() < 1 || ink.height() < 1 || box.width() < 1 || box.height() < 1) continue;
        // A line without descenders or tall letters has little ink for its size, and its box
        // is padded to the line: the box height bounds what the ink alone would overstate.
        sizes.push_back(std::min({PROBE_FONT * box.width() / ink.width(), PROBE_FONT * box.height() / ink.height(), box.height() * SIZE_PER_BOX_HEIGHT}));
    }
    if (sizes.empty()) return std::max(1.0, block.line_height > 0 ? block.line_height : block.rect.height() / std::max(1, block.lines));
    std::nth_element(sizes.begin(), sizes.begin() + sizes.size() / 2, sizes.end());
    return sizes[sizes.size() / 2];
}

// The text broken into lines, the first no wider than widths[0], the next than widths[1] and so
// on; lines past the last width take the last one.
QStringList wrapped(const QString &text, const QFont &font, const QVector<double> &widths) {
    QStringList lines;
    if (widths.isEmpty()) return lines;
    for (const auto &paragraph : text.split('\n')) {
        if (paragraph.trimmed().isEmpty()) continue;
        QTextLayout layout(paragraph, font);
        QTextOption option;
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        layout.setTextOption(option);
        layout.beginLayout();
        for (auto line = layout.createLine(); line.isValid(); line = layout.createLine()) {
            line.setLineWidth(widths[std::min<qsizetype>(lines.size(), widths.size() - 1)]);
            lines << paragraph.mid(line.textStart(), line.textLength()).trimmed();
        }
        layout.endLayout();
    }
    return lines;
}

// Whether a paragraph's lines were set from the left, centred or from the right. Lines as
// wide as the paragraph say nothing; the rest must agree clearly, or left is kept. One short
// first line is an indent, not a centred heading.
Qt::Alignment paragraph_alignment(const OverlayTextBlock &block) {
    if (block.line_rects.size() < 2) return Qt::AlignLeft;
    const double tolerance = 0.15 * std::max(1.0, block.line_height);
    double left = 0, centre = 0, right = 0;
    int telling = 0;
    for (const auto &line : block.line_rects) {
        if (block.rect.width() - line.width() <= 2 * tolerance) continue;
        left += line.left() - block.rect.left();
        right += block.rect.right() - line.right();
        centre += std::abs(line.center().x() - block.rect.center().x());
        ++telling;
    }
    if (telling == 0) return Qt::AlignLeft;
    const double margin = tolerance * telling;
    if (telling == 1 && block.line_rects.first().left() - block.rect.left() > tolerance) return Qt::AlignLeft;
    if (centre + margin < left && centre + margin < right) return Qt::AlignHCenter;
    if (right + margin < left) return Qt::AlignRight;
    return Qt::AlignLeft;
}

// How far a one-line block may run on to its right (or left), up to wanted pixels: over the
// plain page only, and stopping short of the next text. A label is often a word or two, and its
// translation rarely is; shrunk into the label's own width it could not be read.
int room_beside(const QImage &source, const QRect &block, const QColor &background, const QVector<QRect> &other_text, int wanted, bool right) {
    int limit = std::min(wanted, right ? source.width() - 1 - block.right() : block.left());
    const int gap = std::max(4, block.height() / 2);
    for (const auto &other : other_text) {
        if (other.top() > block.bottom() || other.bottom() < block.top()) continue;
        if (right && other.right() > block.right()) limit = std::min(limit, other.left() - block.right() - gap);
        if (!right && other.left() < block.left()) limit = std::min(limit, block.left() - other.right() - gap);
    }
    const QRgb page = background.rgb();
    int room = 0;
    for (; room < limit; ++room) {
        const int x = right ? block.right() + 1 + room : block.left() - 1 - room;
        bool plain = true;
        for (int y = block.top(); y <= block.bottom() && plain; y += 2) plain = channel_distance(source.pixel(x, y), page) <= PLAIN_TOLERANCE;
        if (!plain) break;
    }
    // Not right up against whatever stopped it.
    return room < limit ? std::max(0, room - gap) : std::max(0, room);
}

// The stretch of row free of other text, the one the line itself stands in most: another
// paragraph inside this one's area, such as a box it wraps around, takes its part of the row.
QRectF free_stretch(const QRectF &row, const QRectF &line, const QRegion &other_text) {
    std::vector<std::pair<double, double>> taken;
    for (const auto &rect : other_text) {
        // A box that only touches the row's edge, as the paragraph below often does, takes none of it.
        const double shared = std::min(rect.bottom() + 1.0, row.bottom()) - std::max<double>(rect.top(), row.top());
        if (shared > 0.3 * row.height()) taken.emplace_back(rect.left(), rect.right() + 1);
    }
    if (taken.empty()) return row;
    std::sort(taken.begin(), taken.end());
    QRectF best;
    double best_overlap = -1;
    double from = row.left();
    const auto consider = [&](double left, double right) {
        if (right <= left) return;
        const double overlap = std::max(0.0, std::min(right, line.right()) - std::max(left, line.left()));
        // The line's own stretch first; among stretches clear of it, the widest.
        const double score = overlap > 0 ? row.width() + overlap : right - left;
        if (score > best_overlap) {
            best_overlap = score;
            best = QRectF(left, row.top(), right - left, row.height());
        }
    };
    for (const auto &[left, right] : taken) {
        consider(from, std::min(left, row.right()));
        from = std::max(from, right);
    }
    consider(from, row.right());
    return best;
}

qreal text_width(const QFontMetricsF &metrics, const QString &text, const QRectF &ink) {
    return std::max(metrics.horizontalAdvance(text), ink.right()) - std::min(0.0, ink.left());
}

bool lay_out(OverlayPaintedBlock &entry, const OverlayTextBlock &block, const QString &family, const QVector<QRect> &other_text,
             const std::function<int(int wanted, bool right)> &room) {
    const int cap = std::min(static_cast<int>(std::floor(source_font_size(block))), MAX_OVERLAY_FONT);
    if (cap < 1) return false;
    const bool one_line = block.line_rects.size() == 1;
    // Running on to the left, the translation stays over the label: from its right edge, or
    // centred on it when it also runs on to the right.
    std::optional<Qt::Alignment> one_line_alignment;
    if (one_line) {
        if (entry.text.contains('\n') || entry.text.contains('\r') || entry.text.contains(QChar::LineSeparator)) return false;
        const QFontMetricsF metrics(sized(family, cap));
        const int missing = static_cast<int>(std::ceil(text_width(metrics, entry.text, metrics.tightBoundingRect(entry.text)) - entry.area.width()));
        if (missing > 0) {
            const int right = room(missing, true);
            entry.area.setWidth(entry.area.width() + right);
            entry.cover.setRight(entry.cover.right() + right);
            const int left = right < missing ? room(missing - right, false) : 0;
            if (left > 0) {
                entry.area.setLeft(entry.area.left() - left);
                entry.cover.setLeft(entry.cover.left() - left);
                one_line_alignment = right > 0 ? Qt::AlignHCenter : Qt::AlignRight;
            }
        }
    }

    QRegion protected_text;
    for (const auto &rect : other_text)
        if (entry.cover.intersects(rect)) protected_text += rect.intersected(entry.cover);
    entry.background_region = QRegion(entry.cover).subtracted(protected_text);
    entry.alignment = one_line_alignment.value_or(paragraph_alignment(block));
    QVector<QRectF> rows;
    QVector<double> widths;
    for (const auto &box : block.line_rects) {
        // Reflow the sentence across the paragraph width while keeping its original rows.
        rows.append(free_stretch(QRectF(entry.area.left(), box.top(), entry.area.width(), box.height()).intersected(entry.area), box, protected_text));
        widths.append(rows.last().width());
    }

    for (int low = std::min(MIN_OVERLAY_FONT, cap), high = cap; low <= high;) {
        const int size = (low + high) / 2;
        const auto font = sized(family, size);
        const QFontMetricsF metrics(font);
        const auto lines = one_line ? QStringList{entry.text} : wrapped(entry.text, font, widths);
        QVector<QPointF> origins;
        if (!lines.isEmpty() && lines.size() <= rows.size()) {
            qreal previous_bottom = entry.area.top();
            for (qsizetype index = 0; index < lines.size(); ++index) {
                const auto &row = rows[index];
                const auto ink = metrics.tightBoundingRect(lines[index]);
                const qreal space = row.width() - text_width(metrics, lines[index], ink);
                if (row.isEmpty() || space < 0 || ink.height() > row.height()) break;
                qreal x = row.left() - std::min(0.0, ink.left());
                if (entry.alignment & Qt::AlignRight) x += space;
                else if (entry.alignment & Qt::AlignHCenter) x += space / 2;
                const QPointF origin(x, row.center().y() - ink.center().y());
                const auto placed_ink = ink.translated(origin);
                if (placed_ink.top() < previous_bottom || protected_text.intersects(placed_ink.toAlignedRect())) break;
                previous_bottom = placed_ink.bottom();
                origins.append(origin);
            }
        }
        if (origins.isEmpty() || origins.size() != lines.size()) {
            high = size - 1;
            continue;
        }
        entry.font = font;
        entry.lines = lines;
        entry.line_rects = rows.mid(0, lines.size());
        entry.line_origins = std::move(origins);
        low = size + 1;
    }
    return !entry.lines.isEmpty();
}

struct Line {
    QString text;
    QRectF rect;
    std::optional<OverlayOrientedLine> oriented;
    // For a paragraph being ordered: its place in the list of paragraphs.
    int paragraph = -1;
};

std::optional<OverlayOrientedLine> oriented_line(const OcrLine &line) {
    const auto &box = line.box;
    if (box.size() != 4) return std::nullopt;
    // OCR corners are clockwise. A folded quad cannot be used as a reading surface.
    for (int index = 0; index < 4; ++index) {
        const auto a = box[(index + 1) % 4] - box[index];
        const auto b = box[(index + 2) % 4] - box[(index + 1) % 4];
        const auto turn = a.x() * b.y() - a.y() * b.x();
        if (!std::isfinite(turn) || turn <= 0) return std::nullopt;
    }
    const auto frame = line_frame(box, false);
    if (!frame.valid() || (!line.vertical && frame.along == QPointF(1, 0) && frame.across == QPointF(0, 1))) return std::nullopt;
    const QPolygonF local({{0, 0}, {frame.length, 0}, {frame.length, frame.thickness}, {0, frame.thickness}});
    OverlayOrientedLine result{QPolygonF(box), QSizeF(frame.length, frame.thickness), {}, line.vertical};
    if (!QTransform::quadToQuad(local, result.quad, result.to_image)) return std::nullopt;
    return result;
}

QStringList graphemes(const QString &text) {
    QStringList result;
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
    qsizetype from = 0;
    for (qsizetype to = finder.toNextBoundary(); to >= 0; to = finder.toNextBoundary()) {
        const auto cluster = text.mid(from, to - from);
        if (!cluster.trimmed().isEmpty()) result.append(cluster);
        from = to;
    }
    return result;
}

bool lay_out_vertical(OverlayPaintedBlock &entry, const QString &source, const QString &family) {
    const auto clusters = graphemes(entry.text);
    if (clusters.isEmpty() || entry.text.contains('\n') || entry.text.contains('\r') || entry.text.contains(QChar::LineSeparator)) return false;
    const auto cells = std::max(clusters.size(), graphemes(source).size());
    const qreal pitch = entry.area.height() / cells;
    const int cap = std::min(MAX_OVERLAY_FONT, static_cast<int>(std::floor(std::min(entry.area.width(), pitch) * SIZE_PER_BOX_HEIGHT)));
    for (int size = cap; size >= std::min(MIN_OVERLAY_FONT, cap) && size >= 1; --size) {
        const auto font = sized(family, size);
        const QFontMetricsF metrics(font);
        QVector<QPointF> origins;
        for (qsizetype index = 0; index < clusters.size(); ++index) {
            const auto ink = metrics.tightBoundingRect(clusters[index]);
            if (ink.width() > entry.area.width() || ink.height() > pitch) break;
            origins.append(QPointF(entry.area.center().x(), pitch * (index + 0.5)) - ink.center());
        }
        if (origins.size() != clusters.size()) continue;
        entry.font = font;
        entry.lines = clusters;
        entry.line_origins = std::move(origins);
        return true;
    }
    return false;
}

std::optional<OverlayPaintedBlock> plan_oriented_block(const QImage &source, const QVector<OverlayTextBlock> &blocks, int index,
                                                      const QString &text, const QString &family) {
    const auto &block = blocks[index];
    const auto &line = *block.oriented_line;
    OverlayPaintedBlock entry;
    entry.text = text;
    entry.area = QRectF(QPointF(0, 0), line.size);
    entry.cover = entry.area.toAlignedRect();
    if (line.vertical) {
        if (!lay_out_vertical(entry, block.source, family)) return std::nullopt;
    } else {
        OverlayTextBlock local;
        local.source = block.source;
        local.line_texts = {local.source};
        local.rect = entry.area;
        local.line_rects = {entry.area};
        local.line_height = entry.area.height();
        if (!lay_out(entry, local, family, {}, [](int, bool) { return 0; })) return std::nullopt;
    }

    QRegion cover(line.quad.toPolygon());
    // Keep the antialiased edges of old glyphs inside the cover, including on skewed quads.
    for (int step = 0; step < 2; ++step) {
        cover = cover | cover.translated(-1, 0) | cover.translated(1, 0);
        cover = cover | cover.translated(0, -1) | cover.translated(0, 1);
    }
    cover &= source.rect();
    QRegion neighbours;
    const auto sample_bounds = cover.boundingRect().adjusted(-8, -8, 8, 8);
    for (int other = 0; other < blocks.size(); ++other) {
        if (other == index || !sample_bounds.intersects(blocks[other].rect.toAlignedRect())) continue;
        const auto &nearby = blocks[other];
        neighbours += nearby.oriented_line ? QRegion(nearby.oriented_line->quad.toPolygon()) : QRegion(nearby.rect.toAlignedRect());
    }
    entry.background_region = cover.subtracted(neighbours);
    if (entry.background_region.isEmpty()) return std::nullopt;
    const QFontMetricsF metrics(entry.font);
    for (qsizetype row = 0; row < entry.lines.size(); ++row) {
        const auto ink = metrics.tightBoundingRect(entry.lines[row]).translated(entry.line_origins[row]);
        const QRegion mapped(line.to_image.map(QPolygonF(ink)).toPolygon());
        if (!mapped.subtracted(entry.background_region).isEmpty()) return std::nullopt;
    }
    entry.cover = entry.background_region.boundingRect();
    entry.text_to_image = line.to_image;
    const PixelSource pixels{source, line.to_image, neighbours};
    const auto cells = entry.area.toAlignedRect();
    entry.background = sample_background(pixels, cells);
    entry.foreground = sample_foreground(pixels, cells, entry.background);
    return entry;
}

// A paragraph with its next line added, as it reads: a word the line end broke with a hyphen
// is one word again, and a space goes between the lines except where the script uses none.
QString with_next_line(const QString &paragraph, const QString &next) {
    if (paragraph.isEmpty() || next.isEmpty()) return paragraph + next;
    const QChar last = paragraph.back(), first = next.front();
    if (last == QLatin1Char('-') && paragraph.size() >= 2 && paragraph.at(paragraph.size() - 2).isLetter() && first.isLower())
        return paragraph.left(paragraph.size() - 1) + next;
    if (last.isSpace() || first.isSpace() || is_cjk(last) || is_cjk(first)) return paragraph + next;
    return paragraph + QLatin1Char(' ') + next;
}

// A page's columns are read top to bottom before the next one starts. Pairing a left and a
// right cell row by row hands the translator half-sentences from two different columns, so a
// gutter is found on the x axis first and only then is each band read down its columns. A line
// that spans the gutter (a full-width heading) ends the band instead of joining a column.
void order_in_reading_order(QVector<Line> &lines) {
    const auto top_down = [](const Line &a, const Line &b) {
        if (a.rect.top() != b.rect.top()) return a.rect.top() < b.rect.top();
        return a.rect.left() < b.rect.left();
    };
    std::stable_sort(lines.begin(), lines.end(), top_down);
    // Three rows cannot show a column layout, while a form's label and value columns look like
    // one to any measurement: short pages keep the plain top-down order.
    if (lines.size() < 4) return;

    double left = lines.first().rect.left(), right = lines.first().rect.right();
    std::vector<double> heights;
    heights.reserve(static_cast<size_t>(lines.size()));
    for (const auto &line : lines) {
        left = std::min(left, line.rect.left());
        right = std::max(right, line.rect.right());
        heights.push_back(std::max(1.0, line.rect.height()));
    }
    const auto span = static_cast<int>(std::lround(right - left));
    if (span < 40) return;
    std::nth_element(heights.begin(), heights.begin() + heights.size() / 2, heights.end());
    const double line_height = heights[heights.size() / 2];
    const double gutter_width = std::max(12.0, line_height * 1.6);

    // Two-pixel buckets: fine enough to tell a gutter from the gap between letters, cheap
    // enough for a capture with a thousand lines.
    const int buckets = std::clamp(span / 2, 16, 4096);
    std::vector<int> coverage(static_cast<size_t>(buckets), 0);
    for (const auto &line : lines) {
        const auto first = std::clamp(static_cast<int>((line.rect.left() - left) / 2), 0, buckets - 1);
        const auto last = std::clamp(static_cast<int>((line.rect.right() - left) / 2), 0, buckets - 1);
        for (int index = first; index <= last; ++index) ++coverage[static_cast<size_t>(index)];
    }
    // A heading crossing the gutter must not hide it; a block of body text that spans the page
    // means there is no gutter at all.
    const auto tolerance = std::max(1, static_cast<int>(std::ceil(lines.size() * 0.25)));
    std::vector<double> cuts;
    int run_start = -1;
    for (int index = 0; index <= buckets; ++index) {
        const bool clear = index < buckets && coverage[static_cast<size_t>(index)] <= tolerance;
        if (clear) {
            if (run_start < 0) run_start = index;
            continue;
        }
        if (run_start >= 0) {
            const auto centre = left + (run_start + index);
            // Both sides need room for a column, or the near edge of the page becomes one.
            if ((index - run_start) * 2 >= gutter_width && centre - left >= gutter_width && right - centre >= gutter_width)
                cuts.push_back(centre);
            run_start = -1;
        }
    }
    if (cuts.empty()) return;

    const auto crosses = [&cuts](const Line &line) {
        for (const double cut : cuts)
            if (line.rect.left() < cut && line.rect.right() > cut) return true;
        return false;
    };
    const auto column_of = [&cuts](const Line &line) {
        int column = 0;
        for (const double cut : cuts)
            if (line.rect.center().x() > cut) ++column;
        return column;
    };
    QVector<Line> ordered;
    ordered.reserve(lines.size());
    int index = 0;
    while (index < lines.size()) {
        if (crosses(lines[index])) {
            ordered.append(lines[index++]);
            continue;
        }
        int band_end = index;
        while (band_end < lines.size() && !crosses(lines[band_end])) ++band_end;
        std::stable_sort(lines.begin() + index, lines.begin() + band_end, [&](const Line &a, const Line &b) {
            const int column_a = column_of(a), column_b = column_of(b);
            if (column_a != column_b) return column_a < column_b;
            return top_down(a, b);
        });
        for (int line_index = index; line_index < band_end; ++line_index) ordered.append(lines[line_index]);
        index = band_end;
    }
    lines = ordered;
}

}

QVector<OverlayTextBlock> group_overlay_blocks(const OcrResult &result) {
    QVector<Line> lines;
    lines.reserve(result.data.size());
    for (const auto &item : result.data) {
        const auto text = item.text.trimmed();
        if (text.isEmpty() || item.box.isEmpty()) continue;
        const auto rect = QPolygonF(item.box).boundingRect();
        // A box of no size has nowhere to paint; a single point is the engine's way of
        // saying it has no position for the line.
        if (rect.width() < 2 || rect.height() < 2) continue;
        lines.append({text, rect, oriented_line(item)});
    }
    if (lines.isEmpty()) return {};
    // A line on a rotated or vertical quad is a paragraph of its own.
    QVector<GroupingLine> flat;
    QVector<int> flat_to_line;
    for (int index = 0; index < lines.size(); ++index) {
        if (lines[index].oriented) continue;
        flat.append({lines[index].text, lines[index].rect});
        flat_to_line.append(index);
    }
    QVector<QVector<int>> paragraphs;
    for (const auto &group : group_lines_into_paragraphs(flat)) {
        QVector<int> members;
        for (const int index : group) members.append(flat_to_line[index]);
        paragraphs.append(members);
    }
    for (int index = 0; index < lines.size(); ++index)
        if (lines[index].oriented) paragraphs.append({index});
    // The order the paragraphs are sent in: down each column, then the next column, which is
    // also the order their translations have to be placed in.
    QVector<Line> places;
    for (int paragraph = 0; paragraph < paragraphs.size(); ++paragraph) {
        QRectF rect = lines[paragraphs[paragraph].first()].rect;
        for (const int index : paragraphs[paragraph]) rect = rect.united(lines[index].rect);
        places.append({QString(), rect, std::nullopt, paragraph});
    }
    order_in_reading_order(places);
    QVector<OverlayTextBlock> blocks;
    for (const auto &place : places) {
        OverlayTextBlock block;
        for (const int index : paragraphs[place.paragraph]) {
            const auto &line = lines[index];
            if (block.line_texts.isEmpty()) {
                block.source = line.text;
                block.rect = line.rect;
                block.line_height = line.rect.height();
                block.oriented_line = line.oriented;
            } else {
                block.source = with_next_line(block.source, line.text);
                block.rect = block.rect.united(line.rect);
                block.line_height = std::max(block.line_height, line.rect.height());
            }
            block.line_texts.append(line.text);
            block.line_rects.append(line.rect);
        }
        block.lines = static_cast<int>(block.line_texts.size());
        blocks.append(block);
    }
    return blocks;
}

std::optional<OverlayPaintedBlock> plan_overlay_block(const QImage &source, const QVector<OverlayTextBlock> &blocks, int index, const QString &target_lang,
                                                    bool *does_not_fit) {
    if (does_not_fit) *does_not_fit = false;
    if (source.isNull() || index < 0 || index >= blocks.size()) return std::nullopt;
    const QRect bounds(QPoint(0, 0), source.size());
    auto block = blocks[index];
    const auto text = block.translated.trimmed();
    if (text.isEmpty() || text == block.source.trimmed()) return std::nullopt;
    const auto area = block.rect.intersected(bounds);
    if (area.width() < 2 || area.height() < 2) {
        if (does_not_fit) *does_not_fit = true;
        return std::nullopt;
    }
    const auto family = overlay_font_family(target_lang);
    if (block.oriented_line) {
        auto entry = plan_oriented_block(source, blocks, index, text, family);
        if (does_not_fit) *does_not_fit = !entry;
        return entry;
    }
    // A block built by hand has no line boxes: its lines share its height evenly.
    if (block.line_rects.isEmpty()) {
        const int rows = std::max(1, block.lines);
        for (int row = 0; row < rows; ++row)
            block.line_rects.append(QRectF(block.rect.left(), block.rect.top() + row * block.rect.height() / rows, block.rect.width(), block.rect.height() / rows));
    }
    const double line_height = block.line_height > 0 ? block.line_height : block.rect.height() / std::max(1, block.lines);
    const auto block_rect = area.toAlignedRect();
    QVector<QRect> other_text;
    for (int other = 0; other < blocks.size(); ++other)
        if (other != index) other_text.append(blocks[other].rect.toAlignedRect());
    OverlayPaintedBlock entry;
    entry.area = area;
    const PixelSource pixels{source};
    entry.background = sample_background(pixels, block_rect, other_text);
    entry.foreground = sample_foreground(pixels, block_rect, entry.background);
    entry.cover = ink_cover(source, block_rect, entry.background, entry.foreground, other_text, std::max(1, static_cast<int>(std::lround(line_height / 4))));
    if (entry.cover.isEmpty()) return std::nullopt;
    entry.text = text;
    if (!lay_out(entry, block, family, other_text,
                 [&](int wanted, bool right) { return room_beside(source, block_rect, entry.background, other_text, wanted, right); })) {
        if (does_not_fit) *does_not_fit = true;
        return std::nullopt;
    }
    return entry;
}

namespace {
// Every block that changed, sampled from source before anything is drawn.
QVector<OverlayPaintedBlock> plan_blocks(const QImage &source, const QVector<OverlayTextBlock> &blocks, const QString &target_lang, QVector<int> *unplaced = nullptr) {
    QVector<OverlayPaintedBlock> painted;
    painted.reserve(blocks.size());
    if (unplaced) unplaced->clear();
    for (int index = 0; index < blocks.size(); ++index) {
        bool does_not_fit = false;
        if (const auto entry = plan_overlay_block(source, blocks, index, target_lang, &does_not_fit)) painted.append(*entry);
        else if (unplaced && does_not_fit) unplaced->append(index);
    }
    return painted;
}
}

void paint_overlay_blocks(QImage &target, const QVector<OverlayPaintedBlock> &painted) {
    QPainter painter(&target);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    // Every cover first: a padded one must not paint over a neighbour drawn before it.
    for (const auto &entry : painted)
        for (const auto &rect : entry.background_region) painter.fillRect(rect, entry.background);
    for (const auto &entry : painted) {
        painter.save();
        painter.setClipRegion(entry.background_region);
        if (!entry.text_to_image.isIdentity()) painter.setWorldTransform(entry.text_to_image);
        painter.setPen(entry.foreground);
        painter.setFont(entry.font);
        for (qsizetype line = 0; line < entry.lines.size(); ++line)
            painter.drawText(entry.line_origins[line], entry.lines[line]);
        painter.restore();
    }
}

QImage render_translation_overlay(const QImage &source, const QVector<OverlayTextBlock> &blocks, const QString &target_lang, int *painted_blocks) {
    if (painted_blocks) *painted_blocks = 0;
    const auto painted = plan_blocks(source, blocks, target_lang);
    if (painted.isEmpty()) return {};
    QImage layer(source.size(), QImage::Format_ARGB32_Premultiplied);
    layer.fill(Qt::transparent);
    paint_overlay_blocks(layer, painted);
    if (painted_blocks) *painted_blocks = painted.size();
    return layer;
}

QImage paint_translations(const QImage &source, const QVector<OverlayTextBlock> &blocks, const QString &target_lang, int *painted_blocks, QVector<int> *unplaced) {
    if (painted_blocks) *painted_blocks = 0;
    const auto painted = plan_blocks(source, blocks, target_lang, unplaced);
    if (painted.isEmpty()) return {};
    // Painting detaches the copy: the result is the only new buffer, as large as the source.
    QImage result = source;
    if (result.format() != QImage::Format_RGB32 && result.format() != QImage::Format_ARGB32_Premultiplied)
        result = result.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    paint_overlay_blocks(result, painted);
    if (painted_blocks) *painted_blocks = painted.size();
    return result;
}

namespace {
bool is_short_label(const OverlayTextBlock &block) {
    if (block.lines > 1 || block.line_rects.size() > 1) return false;
    int words = 0, cjk = 0;
    bool in_word = false;
    for (const QChar c : block.source) {
        const bool letter = c.isLetterOrNumber() && !is_cjk(c);
        if (letter && !in_word) ++words;
        in_word = letter;
        if (is_cjk(c) && c.isLetterOrNumber()) ++cjk;
    }
    return words <= 3 && cjk <= 6;
}
}

QString unplaced_notice(const QVector<OverlayTextBlock> &blocks, const QVector<int> &unplaced) {
    const bool longer = std::any_of(unplaced.cbegin(), unplaced.cend(), [&](int index) { return !is_short_label(blocks[index]); });
    return longer ? format_unplaced_translations(blocks, unplaced) : QString();
}

QString format_unplaced_translations(const QVector<OverlayTextBlock> &blocks, const QVector<int> &unplaced) {
    QStringList text;
    for (const int index : unplaced) text.append(blocks[index].source + u'\n' + blocks[index].translated);
    return text.join(QStringLiteral("\n\n"));
}
}
