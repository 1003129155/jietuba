#include "canvas/items/watermark_item.h"
#include "canvas/scene.h"
#include "canvas/selection_model.h"
#include "core/constants.h"
#include <QFontMetricsF>
#include <QGraphicsScene>
#include <QPainter>
#include <QRegularExpression>
#include <QStyleOptionGraphicsItem>
#include <QtMath>
#include <algorithm>
#include <cmath>

namespace jietuba {
const std::vector<std::pair<QString, QPointF>> &watermark_anchors() {
    static const std::vector<std::pair<QString, QPointF>> anchors = {
        {LAYOUT_CENTER, {.5, .5}}, {"top_left", {0, 0}}, {"top", {.5, 0}}, {"top_right", {1, 0}},
        {"bottom_left", {0, 1}}, {"bottom", {.5, 1}}, {"bottom_right", {1, 1}},
    };
    return anchors;
}

const QStringList &watermark_layouts() {
    static const QStringList layouts = [] {
        QStringList result{LAYOUT_TILE};
        for (const auto &anchor : watermark_anchors()) result.append(anchor.first);
        return result;
    }();
    return layouts;
}

namespace {
std::optional<QPointF> anchor_of(const QString &layout) {
    for (const auto &anchor : watermark_anchors())
        if (anchor.first == layout) return anchor.second;
    return std::nullopt;
}

// float(value) of the reference, or nullopt where it raises.
std::optional<double> to_float(const QVariant &value) {
    if (!value.isValid() || value.isNull()) return std::nullopt;
    if (value.metaType().id() == QMetaType::QString) {
        bool ok = false;
        const double number = value.toString().trimmed().toDouble(&ok);
        if (!ok) return std::nullopt;
        return number;
    }
    bool ok = false;
    const double number = value.toDouble(&ok);
    if (!ok) return std::nullopt;
    return number;
}

// Clamped integer with round-half-even; values float() rejects fall back to the default.
int clamp_int(const QVariant &value, int fallback, int low, int high) {
    auto number = to_float(value);
    int result = fallback;
    if (number && std::isfinite(*number)) result = static_cast<int>(std::nearbyint(*number));
    return std::max(low, std::min(high, result));
}

double clamp_float(const QVariant &value, double fallback, double low, double high) {
    const double number = to_float(value).value_or(fallback);
    // min/max with NaN keep the first argument, as Python's do.
    const double upper = number < high ? number : high;
    return upper > low ? upper : low;
}

// Lines of text laid out as one block: each line centred, the block centred on the origin.
struct TextBlock {
    QStringList lines;
    std::vector<double> widths;
    double line_height, ascent, width = 0, height;
    TextBlock(const QString &text, const QFont &font) {
        const QFontMetricsF metrics(font);
        lines = text.split('\n');
        for (const auto &line : lines) {
            widths.push_back(metrics.horizontalAdvance(line));
            width = std::max(width, widths.back());
        }
        line_height = metrics.lineSpacing();
        ascent = metrics.ascent();
        height = metrics.height() + line_height * (lines.size() - 1);
    }
    std::vector<std::pair<QPointF, QString>> baselines() const {
        std::vector<std::pair<QPointF, QString>> result;
        const double top = -height / 2;
        for (qsizetype index = 0; index < lines.size(); ++index)
            result.emplace_back(QPointF(-widths[index] / 2, top + ascent + index * line_height), lines[index]);
        return result;
    }
};

// Tile centres whose box (centre +/- half_w, half_h) meets area. The grid is
// anchored at the selection's top-left with odd rows shifted half a cell, so the
// watermark moves with the selection instead of sliding over the content.
std::vector<QPointF> tile_centers(const QRectF &rect, const QRectF &area, double half_w, double half_h, double cell_w, double cell_h) {
    std::vector<QPointF> centers;
    const auto top = static_cast<long long>(std::ceil((area.top() - half_h - rect.top()) / cell_h - .5));
    const auto bottom = static_cast<long long>(std::floor((area.bottom() + half_h - rect.top()) / cell_h - .5));
    for (auto row = top; row <= bottom; ++row) {
        const double y = rect.top() + (row + .5) * cell_h;
        // Python's % is non-negative for a positive divisor.
        const double shift = ((row % 2) + 2) % 2 ? cell_w / 2 : 0.0;
        const auto left = static_cast<long long>(std::ceil((area.left() - half_w - rect.left() - shift) / cell_w - .5));
        const auto right = static_cast<long long>(std::floor((area.right() + half_w - rect.left() - shift) / cell_w - .5));
        for (auto col = left; col <= right; ++col) centers.emplace_back(rect.left() + shift + (col + .5) * cell_w, y);
    }
    return centers;
}

std::vector<std::pair<QPointF, double>> placements_for(const QRectF &rect, const QString &text, const WatermarkStyle &style,
                                                       const QRectF &exposed, const TextBlock &block) {
    if (text.isEmpty() || rect.isEmpty()) return {};
    if (const auto anchor = anchor_of(style.layout)) {
        const double margin = style.font_size * EDGE_MARGIN;
        const auto inner = rect.adjusted(margin + block.width / 2, margin + block.height / 2,
                                         -margin - block.width / 2, -margin - block.height / 2);
        // A selection narrower than the text falls back to its centre.
        const double x = inner.width() > 0 ? inner.left() + inner.width() * anchor->x() : rect.center().x();
        const double y = inner.height() > 0 ? inner.top() + inner.height() * anchor->y() : rect.center().y();
        return {{QPointF(x, y), 0.0}};
    }
    const auto area = exposed.isNull() ? rect : rect.intersected(exposed);
    if (area.isEmpty()) return {};
    const double rad = qDegreesToRadians(static_cast<double>(std::abs(style.angle)));
    const double box_w = block.width * std::cos(rad) + block.height * std::sin(rad);
    const double box_h = block.width * std::sin(rad) + block.height * std::cos(rad);
    const double gap = style.font_size * style.spacing * SPACING_UNIT;
    const double cell_w = box_w + std::max(gap, 1.0);
    const double cell_h = box_h + std::max(gap * VERTICAL_GAP_RATIO, 1.0);
    // Glyphs may overhang the font metrics box slightly.
    const double pad = style.font_size * GLYPH_OVERHANG;
    std::vector<std::pair<QPointF, double>> result;
    for (const auto &center : tile_centers(rect, area, box_w / 2 + pad, box_h / 2 + pad, cell_w, cell_h))
        result.emplace_back(center, static_cast<double>(style.angle));
    return result;
}
}

WatermarkStyle WatermarkStyle::from_values(const QVariantMap &values) {
    const WatermarkStyle defaults;
    WatermarkStyle style;
    const QColor color(values.value("color", defaults.color).toString());
    style.color = color.isValid() ? color.name() : defaults.color;
    style.font_size = clamp_int(values.value("font_size", defaults.font_size), defaults.font_size, MIN_FONT_SIZE, MAX_FONT_SIZE);
    style.opacity = clamp_float(values.value("opacity", defaults.opacity), defaults.opacity, 0.0, 1.0);
    const auto layout = values.value("layout", defaults.layout).toString();
    style.layout = watermark_layouts().contains(layout) ? layout : LAYOUT_TILE;
    style.spacing = clamp_int(values.value("spacing", defaults.spacing), defaults.spacing, MIN_SPACING, MAX_SPACING);
    style.angle = clamp_int(values.value("angle", defaults.angle), defaults.angle, MIN_ANGLE, MAX_ANGLE);
    style.font_family = values.value("font_family").toString();
    return style;
}

QVariantMap WatermarkStyle::to_values() const {
    return {{"color", color}, {"font_size", font_size}, {"opacity", opacity}, {"layout", layout},
            {"spacing", spacing}, {"angle", angle}, {"font_family", font_family}};
}

WatermarkStyle WatermarkStyle::with_changes(const QVariantMap &changes) const {
    auto values = to_values();
    for (auto it = changes.begin(); it != changes.end(); ++it) values.insert(it.key(), it.value());
    return from_values(values);
}

bool WatermarkStyle::operator==(const WatermarkStyle &o) const {
    return color == o.color && font_size == o.font_size && opacity == o.opacity && layout == o.layout
        && spacing == o.spacing && angle == o.angle && font_family == o.font_family;
}

QString expand_text(const QString &template_text, const QDateTime &when) {
    const auto moment = when.isValid() ? when : QDateTime::currentDateTime();
    static const QRegularExpression token(R"(\$([^$\n]*)\$)");
    QString result;
    qsizetype last = 0;
    auto it = token.globalMatch(template_text);
    while (it.hasNext()) {
        const auto match = it.next();
        result += template_text.mid(last, match.capturedStart() - last);
        const auto pattern = match.captured(1);
        result += pattern.isEmpty() ? QString("$") : moment.toString(pattern);
        last = match.capturedEnd();
    }
    result += template_text.mid(last);
    return result.trimmed();
}

QFont watermark_font(const WatermarkStyle &style) {
    QFont font(style.font_family.isEmpty() ? get_default_text_font_for_language() : style.font_family);
    font.setPixelSize(style.font_size);
    return font;
}

std::vector<std::pair<QPointF, double>> watermark_placements(const QRectF &rect, const QString &text, const WatermarkStyle &style,
                                                             const QRectF &exposed, const std::optional<QFont> &font) {
    if (text.isEmpty() || rect.isEmpty()) return {};
    const TextBlock block(text, font ? *font : watermark_font(style));
    return placements_for(rect, text, style, exposed, block);
}

void paint_watermark(QPainter *painter, const QRectF &rect, const QString &text, const WatermarkStyle &style,
                     const QRectF &exposed, const std::optional<QFont> &font) {
    if (text.isEmpty()) return;
    const auto used_font = font ? *font : watermark_font(style);
    const TextBlock block(text, used_font);
    const auto placements = placements_for(rect, text, style, exposed, block);
    if (placements.empty()) return;
    painter->save();
    painter->setClipRect(rect, Qt::IntersectClip);
    painter->setRenderHint(QPainter::TextAntialiasing, true);
    painter->setFont(used_font);
    // Opacity goes into the pen colour: painter opacity sends text down a path about five times slower.
    QColor color(style.color);
    color.setAlphaF(style.opacity);
    painter->setPen(color);
    const auto base = painter->transform();
    const auto lines = block.baselines();
    for (const auto &[center, angle] : placements) {
        QTransform transform(base);
        transform.translate(center.x(), center.y());
        transform.rotate(angle);
        painter->setTransform(transform);
        for (const auto &[origin, line] : lines) painter->drawText(origin, line);
    }
    painter->restore();
}

QImage apply_watermark(const QImage &image, const QString &text, const WatermarkStyle &style) {
    if (image.isNull() || text.isEmpty()) return image;
    auto result = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QPainter painter(&result);
    painter.setRenderHint(QPainter::Antialiasing, true);
    paint_watermark(&painter, QRectF(result.rect()), text, style);
    painter.end();
    return result;
}

WatermarkItem::WatermarkItem(const QString &template_text, const WatermarkStyle &style, const QDateTime &created_at)
    : TrackedItem(this), created_at_(created_at.isValid() ? created_at : QDateTime::currentDateTime()), template_(template_text),
      text_(expand_text(template_, created_at_)), style_(style.normalized()), font_(watermark_font(style_)) {
    setZValue(Z_VALUE);
    setAcceptedMouseButtons(Qt::NoButton);
    // exposedRect lets partial repaints draw only the intersecting copies.
    setFlag(ItemUsesExtendedStyleOption, true);
}

WatermarkItem *WatermarkItem::find(QGraphicsScene *scene) {
    if (!scene) return nullptr;
    for (auto *item : scene->items())
        if (auto *watermark = dynamic_cast<WatermarkItem *>(item)) return watermark;
    return nullptr;
}

void WatermarkItem::set_template(const QString &template_text, const QDateTime &when) {
    if (template_text == template_ && !when.isValid()) return;
    template_ = template_text;
    if (when.isValid()) created_at_ = when;
    text_ = expand_text(template_text, created_at_);
    update();
}

void WatermarkItem::set_style(const WatermarkStyle &style) {
    const auto normalized = style.normalized();
    if (normalized != style_) {
        style_ = normalized;
        font_ = watermark_font(normalized);
        update();
    }
}

void WatermarkItem::set_scene_rect(const QRectF &rect) {
    const auto local = mapRectFromScene(rect);
    if (local != rect_) {
        prepareGeometryChange();
        rect_ = local;
    }
}

QVariant WatermarkItem::itemChange(GraphicsItemChange change, const QVariant &value) {
    if (change == ItemSceneHasChanged)
        if (auto *canvas = dynamic_cast<CanvasScene *>(scene())) set_scene_rect(canvas->selection_model->rect());
    return QGraphicsItem::itemChange(change, value);
}

void WatermarkItem::paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *) {
    const auto exposed = option ? option->exposedRect : QRectF();
    paint_watermark(painter, rect_, text_, style_, exposed.isEmpty() ? QRectF() : exposed, font_);
}
}
