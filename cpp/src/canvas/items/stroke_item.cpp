#include "canvas/items/stroke_item.h"
#include <QPainter>
#include <QPainterPathStroker>
#include <QStyleOptionGraphicsItem>
#include <algorithm>
#include <cmath>
#include <tuple>
#include <utility>

namespace jietuba {
namespace {
// Appends source's segments from element start onto target, at most limit of them; returns the
// element it stopped at and how many segments it appended.
std::pair<int, int> append_segments(QPainterPath &target, const QPainterPath &source, int start, int limit) {
    const int count = source.elementCount();
    int i = start, segments = 0;
    while (i < count && segments < limit) {
        const auto element = source.elementAt(i);
        if (element.isCurveTo()) {
            if (i + 2 >= count) break;
            const auto c2 = source.elementAt(i + 1), end = source.elementAt(i + 2);
            target.cubicTo(element.x, element.y, c2.x, c2.y, end.x, end.y);
            i += 3;
        } else if (element.isMoveTo()) {
            target.moveTo(element.x, element.y);
            i += 1;
        } else {
            target.lineTo(element.x, element.y);
            i += 1;
        }
        ++segments;
    }
    return {i, segments};
}

// What the pen draws depends only on these; a colour change keeps the chunks and dash outlines.
auto outline_key(const QPen &pen) {
    return std::make_tuple(pen.widthF(), pen.style(), pen.dashPattern(), pen.dashOffset(), pen.capStyle(),
                           pen.joinStyle(), pen.miterLimit(), pen.isCosmetic());
}

// Ordered max/min preserve Python's treatment of NaN as the second argument.
qreal opacity_value(qreal value) { return std::max(0., std::min(1., value)); }
}

StrokeItem::StrokeItem(const QPainterPath &path, const QPen &pen, bool highlighter)
    : DrawingItemMixin(this), is_highlighter(highlighter), path_(path) {
    setFlag(ItemUsesExtendedStyleOption, true);
    // Highlighter strokes sit under pen strokes.
    setZValue(highlighter ? 10 : 20);
    QAbstractGraphicsShapeItem::setPen(pen);
    bounds_ = exact_bounds();
}

void StrokeItem::setPath(const QPainterPath &path) {
    prepareGeometryChange();
    path_ = path;
    chunks_.reset();
    shape_cache_.reset();
    bounds_ = exact_bounds();
    update();
}

void StrokeItem::extend_path(const QPainterPath &source, int dirty_from) {
    const int count = path_.elementCount();
    for (int i = std::min(dirty_from, count); i < count; ++i) {
        const auto element = source.elementAt(i);
        path_.setElementPositionAt(i, element.x, element.y);
    }
    append_segments(path_, source, count, source.elementCount());
    shape_cache_.reset();

    if (!chunks_) chunks_.emplace();
    auto &chunks = *chunks_;
    // The changes are all at the end; search from there.
    size_t keep = chunks.size();
    while (keep && chunks[keep - 1].end > dirty_from) --keep;
    // A last chunk short of full with new elements after it is cut again with them, not followed by a small one.
    if (keep && chunks[keep - 1].segments < CHUNK_SEGMENTS && chunks[keep - 1].end < path_.elementCount()) --keep;
    QRectF dirty;
    for (size_t k = keep; k < chunks.size(); ++k) dirty = dirty.united(chunks[k].rect);
    const int start = keep ? chunks[keep - 1].end : 1;
    const qreal travelled = keep ? chunks[keep - 1].travelled + chunks[keep - 1].length : 0;
    chunks.resize(keep);
    auto fresh = build_chunks(start, travelled);
    for (const auto &chunk : fresh) dirty = dirty.united(chunk.rect);
    chunks.insert(chunks.end(), std::make_move_iterator(fresh.begin()), std::make_move_iterator(fresh.end()));

    if (dirty.isEmpty()) return;
    if (!bounds_.contains(dirty)) {
        prepareGeometryChange();
        bounds_ = bounds_.united(dirty).adjusted(-GROW_MARGIN, -GROW_MARGIN, GROW_MARGIN, GROW_MARGIN);
    }
    update(dirty);
}

void StrokeItem::finish_path() {
    const QRectF bounds = exact_bounds();
    if (bounds == bounds_) return;
    prepareGeometryChange();
    bounds_ = bounds;
}

void StrokeItem::setPen(const QPen &pen) {
    const bool reshaped = outline_key(pen) != outline_key(this->pen());
    QAbstractGraphicsShapeItem::setPen(pen);
    if (!reshaped) return;
    chunks_.reset();
    shape_cache_.reset();
    const QRectF bounds = exact_bounds();
    if (bounds == bounds_) return;
    prepareGeometryChange();
    bounds_ = bounds;
}

bool StrokeItem::dashed() const {
    const auto style = pen().style();
    return style != Qt::SolidLine && style != Qt::NoPen;
}

qreal StrokeItem::paint_reach() const {
    const QPen pen = this->pen();
    qreal reach = pen.widthF() / 2;
    if (pen.capStyle() == Qt::SquareCap) reach *= std::sqrt(2.0);
    if (pen.joinStyle() == Qt::MiterJoin || pen.joinStyle() == Qt::SvgMiterJoin)
        reach = std::max(reach, pen.widthF() * pen.miterLimit());
    return reach + 2;
}

QRectF StrokeItem::exact_bounds() const {
    if (path_.isEmpty()) return {};
    // The hit area's round caps and offset curves are approximations that bulge slightly past the
    // radius; 1 px more covers them.
    const qreal reach = std::max(paint_reach(), pen().widthF() / 2 + HIT_MARGIN + 1);
    return path_.boundingRect().adjusted(-reach, -reach, reach, reach);
}

std::vector<StrokeItem::Chunk> StrokeItem::build_chunks(int start, qreal travelled) const {
    std::vector<Chunk> chunks;
    const qreal reach = paint_reach();
    int i = start;
    while (i < path_.elementCount()) {
        const auto origin = path_.elementAt(i - 1);
        QPainterPath sub(QPointF(origin.x, origin.y));
        const auto [end, segments] = append_segments(sub, path_, i, CHUNK_SEGMENTS);
        if (end == i) break;
        Chunk chunk{i, end, segments, sub, sub.controlPointRect().adjusted(-reach, -reach, reach, reach), travelled,
                    sub.length(), std::nullopt};
        travelled += chunk.length;
        chunks.push_back(std::move(chunk));
        i = end;
    }
    return chunks;
}

const std::vector<StrokeItem::Chunk> &StrokeItem::chunks() const {
    if (!chunks_) chunks_ = build_chunks(1, 0);
    return *chunks_;
}

QPainterPath StrokeItem::shape() const {
    if (shape_cache_) return *shape_cache_;
    if (path_.isEmpty()) return {};
    QPainterPathStroker stroker;
    stroker.setWidth(pen().widthF() + 2 * HIT_MARGIN);
    stroker.setCapStyle(Qt::RoundCap);
    stroker.setJoinStyle(Qt::RoundJoin);
    shape_cache_ = stroker.createStroke(path_);
    return *shape_cache_;
}

void StrokeItem::paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *) {
    if (path_.elementCount() < 2) return;
    // Highlighters multiply onto what lies under them.
    if (is_highlighter) painter->setCompositionMode(QPainter::CompositionMode_Multiply);
    painter->setRenderHint(QPainter::Antialiasing);
    QRectF exposed = option ? option->exposedRect : QRectF();
    // Painting a preview with a fresh QStyleOptionGraphicsItem leaves the exposed area unset.
    if (exposed.isNull()) exposed = bounds_;
    if (dashed()) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(pen().brush());
        painter->drawPath(dash_outline(exposed));
    } else {
        painter->setPen(pen());
        painter->setBrush(Qt::NoBrush);
        painter->drawPath(visible_path(exposed));
    }
}

QPainterPath StrokeItem::visible_path(const QRectF &exposed) const {
    if (exposed.contains(bounds_)) return path_;
    QPainterPath merged;
    int joined_at = -1;
    for (const auto &chunk : chunks()) {
        if (!chunk.rect.intersects(exposed)) continue;
        if (chunk.first == joined_at) merged.connectPath(chunk.path);
        else merged.addPath(chunk.path);
        joined_at = chunk.end;
    }
    return merged;
}

QPainterPath StrokeItem::dash_outline(const QRectF &exposed) const {
    const bool whole = exposed.contains(bounds_);
    const QPen pen = this->pen();
    const qreal width = pen.widthF() > 0 ? pen.widthF() : 1;
    QPainterPath merged;
    merged.setFillRule(Qt::WindingFill);
    for (const auto &chunk : chunks()) {
        if (!whole && !chunk.rect.intersects(exposed)) continue;
        if (!chunk.outline) {
            QPainterPathStroker stroker(pen);
            // Dash offsets are in pen widths.
            stroker.setDashOffset(pen.dashOffset() + chunk.travelled / width);
            chunk.outline = stroker.createStroke(chunk.path);
        }
        merged.addPath(*chunk.outline);
    }
    return merged;
}

void StrokeItem::set_stroke_width(qreal width) {
    auto pen = this->pen();
    pen.setWidthF(std::max(1., width));
    setPen(pen);
}
bool StrokeItem::scale_stroke_width(qreal scale) {
    set_stroke_width(pen().widthF() * scale);
    return true;
}
bool StrokeItem::set_visual_opacity(qreal opacity) {
    auto pen = this->pen();
    auto color = pen.color();
    color.setAlphaF(opacity_value(opacity));
    pen.setColor(color);
    setPen(pen);
    setOpacity(1.);
    return true;
}
// Highlighter pens are built three times the set width.
std::optional<qreal> StrokeItem::get_stroke_width() const { return pen().widthF() / (is_highlighter ? 3. : 1.); }
std::optional<qreal> StrokeItem::get_visual_opacity() const {
    const auto direct = opacity_value(opacity());
    return direct < .999 ? direct : pen().color().alphaF();
}
}
