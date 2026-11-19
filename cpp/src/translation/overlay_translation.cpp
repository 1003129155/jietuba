#include "translation/overlay_translation.h"
#include "canvas/items/background_item.h"
#include "canvas/scene.h"
#include <QFont>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QPainter>
#include <QPointer>
#include <QPolygonF>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace jietuba {
namespace {
// Paragraph joins: a line this close to the one above, and this nearly flush with its left
// edge, continues it. Both are fractions of the line height, so the same rule holds at
// every font size and DPI.
constexpr double JOIN_GAP_BELOW = -0.2;
constexpr double JOIN_GAP_ABOVE = 0.8;
constexpr double JOIN_LEFT_TOLERANCE = 0.65;
// Longest paragraph kept: a run of unrelated lines that happens to line up stays readable.
constexpr int MAX_PARAGRAPH_LINES = 12;

// Smallest size a translation is still drawn at; anything smaller is unreadable, and the
// text is clipped to its block instead.
constexpr int MIN_OVERLAY_FONT = 6;
constexpr int MAX_OVERLAY_FONT = 96;

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

// The colour of the page behind the text: the median around the block's border, which
// skips the glyphs themselves wherever they touch it.
QColor sample_background(const QImage &image, const QRect &bounds) {
    std::vector<QRgb> samples;
    if (bounds.isEmpty()) return {};
    const int step_x = std::max(1, bounds.width() / 24);
    const int step_y = std::max(1, bounds.height() / 16);
    for (int x = bounds.left(); x < bounds.right(); x += step_x) {
        samples.push_back(image.pixel(x, bounds.top()));
        samples.push_back(image.pixel(x, bounds.bottom() - 1));
    }
    for (int y = bounds.top(); y < bounds.bottom(); y += step_y) {
        samples.push_back(image.pixel(bounds.left(), y));
        samples.push_back(image.pixel(bounds.right() - 1, y));
    }
    return median_color(samples);
}

// The colour the original text was drawn in: the median of the pixels standing out most from
// the background, which are the glyph cores rather than their antialiased edges. A textless
// block keeps a plain black or white, whichever the background asks for.
QColor sample_foreground(const QImage &image, const QRect &bounds, const QColor &background) {
    std::vector<std::pair<QRgb, int>> candidates;
    if (!bounds.isEmpty()) {
        const int step = std::max(1, static_cast<int>(std::sqrt(bounds.width() * double(bounds.height()) / 1500.0)));
        for (int y = bounds.top(); y < bounds.bottom(); y += step) {
            for (int x = bounds.left(); x < bounds.right(); x += step) {
                const auto color = image.pixel(x, y);
                const int contrast = std::abs(qRed(color) - background.red()) + std::abs(qGreen(color) - background.green()) +
                                     std::abs(qBlue(color) - background.blue());
                if (contrast > 180) candidates.emplace_back(color, contrast);
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

// A font that shows the target language well: CJK needs its own faces, Latin text is set in
// the UI face. Falls back to the application font when none of them is installed.
QString overlay_font_family(const QString &target_lang) {
    const auto lang = target_lang.toUpper();
    const bool cjk = lang.startsWith("ZH") || lang.startsWith("JA") || lang.startsWith("KO");
    const QStringList wanted = cjk ? QStringList{"Microsoft YaHei UI", "Microsoft YaHei", "SimHei", "Segoe UI"}
                                   : QStringList{"Segoe UI", "Arial", "Microsoft YaHei UI"};
    for (const auto &family : wanted) {
        if (QFontInfo(QFont(family)).family().compare(family, Qt::CaseInsensitive) == 0) return family;
    }
    return {};
}

bool text_fits(const QString &text, const QFont &font, const QSizeF &area) {
    const QFontMetricsF metrics(font);
    const auto bounds = metrics.boundingRect(QRectF(0, 0, area.width(), 1e6), Qt::TextWordWrap | Qt::AlignLeft, text);
    return bounds.height() <= area.height() + 0.5 && bounds.width() <= area.width() + 0.5;
}

// The largest size of the block's own line height at which the translation still fits: a
// translation longer than the original shrinks rather than pushing into its neighbours.
QFont fitted_font(const QString &text, const QSizeF &area, double line_height, const QString &family) {
    double low = MIN_OVERLAY_FONT;
    // The OCR box hugs the glyphs, so the original size is that much larger than the box.
    double high = std::clamp(line_height * 1.05, double(MIN_OVERLAY_FONT), double(MAX_OVERLAY_FONT));
    for (int step = 0; step < 8; ++step) {
        const double candidate = (low + high) / 2;
        QFont probe(family);
        probe.setPixelSize(std::max(MIN_OVERLAY_FONT, static_cast<int>(std::lround(candidate))));
        if (text_fits(text, probe, area)) low = candidate;
        else high = candidate;
    }
    QFont font(family);
    font.setPixelSize(std::max(MIN_OVERLAY_FONT, static_cast<int>(std::lround(low))));
    return font;
}

// A translation is often longer than the line it replaces, and squeezing it into the old
// line's height makes it unreadable. A block may therefore take the blank room below it,
// up to the next block: only when those pixels are plainly the same background, so nothing
// (an icon, a picture, another column's text) is painted over.
int growth_room(const QImage &source, const QRect &block_rect, const QColor &background, int next_top, int padding, double line_height) {
    if (next_top <= block_rect.bottom() + 4) return 0;
    const auto limit = std::min(next_top - padding, block_rect.bottom() + static_cast<int>(std::lround(line_height * 2.5)));
    const auto strip = QRect(block_rect.left(), block_rect.bottom(), block_rect.width(), limit - block_rect.bottom())
                           .intersected(QRect(QPoint(0, 0), source.size()));
    if (strip.height() < 4 || strip.width() < 4) return 0;
    const int step_x = std::max(1, strip.width() / 32);
    const int step_y = std::max(1, strip.height() / 12);
    int total = 0, plain = 0;
    for (int y = strip.top(); y < strip.bottom(); y += step_y) {
        for (int x = strip.left(); x < strip.right(); x += step_x) {
            const auto color = source.pixel(x, y);
            ++total;
            if (std::abs(qRed(color) - background.red()) + std::abs(qGreen(color) - background.green()) + std::abs(qBlue(color) - background.blue()) <= 30)
                ++plain;
        }
    }
    return total > 0 && plain * 100 >= total * 97 ? strip.height() : 0;
}

// One step of the undo stack. Only the translated region is kept for undo: a copy of the
// whole desktop per step would be tens of megabytes. Swapping the pixels there means the
// translation also reaches pins, the magnifier and mosaic sampling, which all read them.
class ReplaceBackgroundRegionCommand : public QUndoCommand {
public:
    ReplaceBackgroundRegionCommand(CanvasScene *scene, QRect region, QImage before, QImage after, const QString &text)
        : QUndoCommand(text), scene_(scene), region_(std::move(region)), before_(std::move(before)), after_(std::move(after)) {}
    void undo() override { apply(before_); }
    void redo() override { apply(after_); }
private:
    void apply(const QImage &patch) {
        if (!scene_ || !scene_->background) return;
        auto image = scene_->background->image();
        if (image.isNull()) return;
        {
            QPainter painter(&image);
            // A plain overwrite: the patch is the exact pixels that region had before.
            painter.setCompositionMode(QPainter::CompositionMode_Source);
            painter.drawImage(region_.topLeft(), patch);
        }
        scene_->background->update_image(image);
    }
    QPointer<CanvasScene> scene_;
    QRect region_;
    QImage before_, after_;
};

struct Line {
    QString text;
    QRectF rect;
};

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

struct PaintedBlock {
    QRectF area;
    QRect cover;
    QColor background;
    QColor foreground;
    QFont font;
    QString text;
};
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
        lines.append({text, rect});
    }
    if (lines.isEmpty()) return {};
    // The order the paragraphs are sent in: down each column, then the next column, which is
    // also the order their translations have to be placed in.
    order_in_reading_order(lines);
    QVector<OverlayTextBlock> blocks;
    for (const auto &line : lines) {
        if (!blocks.isEmpty()) {
            auto &previous = blocks.last();
            const double line_height = previous.line_height > 0 ? previous.line_height : previous.rect.height() / std::max(1, previous.lines);
            const double gap = line.rect.top() - previous.rect.bottom();
            const bool same_margin = std::abs(line.rect.left() - previous.rect.left()) <= line_height * JOIN_LEFT_TOLERANCE;
            const bool follows = gap >= line_height * JOIN_GAP_BELOW && gap <= line_height * JOIN_GAP_ABOVE;
            // A heading is set in its own, larger size: the next line is a new block even
            // when it starts at the same margin.
            const double size_ratio = line.rect.height() / std::max(1.0, line_height);
            const bool same_size = size_ratio > 0.7 && size_ratio < 1.4;
            // A wrapped paragraph continues at the same margin, one line height down; a
            // label above, a new column beside it or a gap stays its own block.
            if (same_margin && follows && same_size && previous.lines < MAX_PARAGRAPH_LINES) {
                previous.source += QLatin1Char('\n') + line.text;
                previous.rect = previous.rect.united(line.rect);
                previous.line_height = std::max(previous.line_height, line.rect.height());
                ++previous.lines;
                continue;
            }
        }
        OverlayTextBlock block;
        block.source = line.text;
        block.rect = line.rect;
        block.line_height = line.rect.height();
        blocks.append(block);
    }
    return blocks;
}

QImage render_translation_overlay(const QImage &source, const QVector<OverlayTextBlock> &blocks, const QString &target_lang, int *painted_blocks) {
    if (painted_blocks) *painted_blocks = 0;
    if (source.isNull() || blocks.isEmpty()) return {};
    const QRect bounds(QPoint(0, 0), source.size());
    const QString family = overlay_font_family(target_lang);
    QVector<PaintedBlock> painted;
    painted.reserve(blocks.size());
    for (int index = 0; index < blocks.size(); ++index) {
        const auto &block = blocks[index];
        const auto text = block.translated.trimmed();
        if (text.isEmpty() || text == block.source.trimmed()) continue;
        auto area = block.rect.intersected(bounds);
        if (area.width() < 2 || area.height() < 2) continue;
        // The paragraph's own line size, which is what the translation is set to: the pitch
        // between lines is larger, so this never makes the new text bigger than the old.
        const double line_height = block.line_height > 0 ? block.line_height : block.rect.height() / std::max(1, block.lines);
        // A little over the text box, so no antialiased edge of the original glyphs survives
        // beside the new text; neighbours that overlap are covered in the same pass.
        const int padding = std::clamp(static_cast<int>(std::lround(line_height * 0.18)), 2, 8);
        const auto block_rect = area.toAlignedRect();
        auto cover = block_rect.adjusted(-padding, -padding, padding, padding).intersected(bounds);
        if (cover.isEmpty()) continue;
        const auto background = sample_background(source, cover);
        auto font = fitted_font(text, area.size(), line_height, family);
        // Too small to read: the translation needs more lines than the original had. It may
        // take the blank room below, which is then covered as well.
        if (font.pixelSize() < line_height * 0.8) {
            const int next_top = index + 1 < blocks.size() ? static_cast<int>(std::lround(blocks[index + 1].rect.top())) : bounds.bottom();
            const int room = growth_room(source, block_rect, background, next_top, padding, line_height);
            if (room > 0) {
                const auto grown = QRectF(area.left(), area.top(), area.width(), area.height() + room);
                const auto grown_font = fitted_font(text, grown.size(), line_height, family);
                if (grown_font.pixelSize() > font.pixelSize()) {
                    area = grown;
                    font = grown_font;
                    cover = block_rect.adjusted(-padding, -padding, padding, padding + room).intersected(bounds);
                }
            }
        }
        PaintedBlock entry;
        entry.area = area;
        entry.cover = cover;
        entry.background = background;
        entry.foreground = sample_foreground(source, block_rect, background);
        entry.font = font;
        entry.text = text;
        painted.append(entry);
    }
    if (painted.isEmpty()) return {};
    QImage layer(source.size(), QImage::Format_ARGB32_Premultiplied);
    layer.fill(Qt::transparent);
    QPainter painter(&layer);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    // Every cover first: a padded one must not paint over a neighbour drawn before it.
    for (const auto &entry : painted) painter.fillRect(entry.cover, entry.background);
    for (const auto &entry : painted) {
        painter.save();
        // Clipped to its own block, so a translation that cannot shrink enough is cut off
        // instead of running over the text beside it.
        painter.setClipRect(QRectF(entry.cover));
        painter.setPen(entry.foreground);
        painter.setFont(entry.font);
        painter.drawText(entry.area, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap, entry.text);
        painter.restore();
    }
    painter.end();
    if (painted_blocks) *painted_blocks = painted.size();
    return layer;
}

void apply_translation_overlay(CanvasScene *scene, const QImage &layer, const QPoint &offset, const QString &command_text) {
    if (!scene || !scene->background || !scene->undo_stack || layer.isNull()) return;
    const auto background = scene->background->image();
    if (background.isNull()) return;
    const auto region = QRect(offset, layer.size()).intersected(QRect(QPoint(0, 0), background.size()));
    if (region.isEmpty()) return;
    const auto before = background.copy(region);
    // The layer is transparent wherever nothing was rewritten, so it is laid over the region
    // rather than replacing it.
    auto after = before;
    {
        QPainter painter(&after);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(QPoint(0, 0), layer, QRect(offset - region.topLeft(), region.size()));
    }
    scene->undo_stack->push(new ReplaceBackgroundRegionCommand(scene, region, before, after, command_text));
}
}
