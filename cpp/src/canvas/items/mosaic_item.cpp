#include "canvas/items/mosaic_item.h"
#include <QPainter>
#include <QPainterPathStroker>
#include <algorithm>

namespace jietuba {
MosaicItem::MosaicItem(const QPainterPath &path, qreal brush_width, int block_size, const QImage &reduced_image,
                       const QRectF &background_rect, bool fill_mode, bool smooth)
    : QGraphicsItem(), DrawingItemMixin(this), brush_width_(std::max(1.0, brush_width)), block_size_(std::max(2, block_size)),
      reduced_image_(reduced_image), background_rect_(background_rect), fill_mode_(fill_mode), smooth_(smooth) {
    setZValue(5);
    // Selectable and movable like other strokes; box mode also gets the generic
    // eight-handle resize, which handle_editor drives through rect()/setRect().
    set_path(path);
}

void MosaicItem::set_smooth(bool smooth) {
    if (smooth == smooth_) return;
    smooth_ = smooth;
    update();
}

void MosaicItem::set_block_size(int block_size, const QImage &reduced_image) {
    block_size = std::max(2, block_size);
    if (block_size == block_size_) return;
    block_size_ = block_size;
    reduced_image_ = reduced_image;
    update();
}

void MosaicItem::setRect(const QRectF &rect) {
    if (!fill_mode_) return;
    QPainterPath path;
    path.addRect(rect);
    set_path(path);
}

QPainterPath MosaicItem::make_shape(const QPainterPath &path) const {
    if (fill_mode_) return path;
    if (path.elementCount() == 0) return {};
    QPainterPathStroker stroker;
    stroker.setWidth(brush_width_);
    stroker.setCapStyle(Qt::RoundCap);
    stroker.setJoinStyle(Qt::RoundJoin);
    auto shape = stroker.createStroke(path);
    // Round caps already cover the start point.
    if (!shape.isEmpty()) return shape;
    // A press without movement strokes to nothing; fall back to a dot.
    const auto first = path.elementAt(0);
    const auto radius = brush_width_ / 2.0;
    QPainterPath stamp;
    stamp.addEllipse(QPointF(first.x, first.y), radius, radius);
    return stamp;
}

void MosaicItem::set_path(const QPainterPath &path) {
    prepareGeometryChange();
    path_ = path;
    shape_cache_.reset();
    if (fill_mode_) {
        rect_ = path_.boundingRect();
        bounds_ = rect_;
    } else {
        // Round-capped stroke: the path grown by half the brush width.
        const auto half = brush_width_ / 2.0;
        bounds_ = path_.boundingRect().adjusted(-half, -half, half, half);
    }
    update();
}

QPainterPath MosaicItem::shape() const {
    if (!shape_cache_) shape_cache_ = make_shape(path_);
    return *shape_cache_;
}

void MosaicItem::translate_background_anchor(qreal dx, qreal dy) {
    background_rect_.translate(dx, dy);
    update();
}

void MosaicItem::paint(QPainter *painter, const QStyleOptionGraphicsItem *, QWidget *) {
    // elementCount, not isEmpty(): a lone moveTo is "empty" but must paint a dot.
    if (reduced_image_.isNull() || path_.elementCount() == 0) return;
    // The painter carries this item's transform; the ink must stay pinned to the
    // background's scene position, so the texture is placed in scene coordinates
    // and then mapped back through the inverse scene transform.
    bool invertible = false;
    const auto to_local = sceneTransform().inverted(&invertible);
    if (!invertible) return;
    // The reduced image has a one-pixel replicated border; step back one block.
    const qreal block = block_size_;
    QBrush brush(reduced_image_);
    brush.setTransform(QTransform::fromScale(block, block)
                       * QTransform::fromTranslate(background_rect_.x() - block, background_rect_.y() - block)
                       * to_local);
    painter->save();
    // The tiled brush repeats past the background edge; clip by the background
    // polygon in local coordinates (a rotated item would leak at the corners with a rect).
    QPainterPath clip;
    clip.addPolygon(mapFromScene(background_rect_));
    painter->setClipPath(clip, Qt::IntersectClip);
    painter->setRenderHint(QPainter::SmoothPixmapTransform, smooth_);
    if (fill_mode_ || path_.elementCount() == 1) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(brush);
        painter->drawPath(fill_mode_ ? path_ : shape());
    } else {
        painter->setPen(QPen(brush, brush_width_, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->setBrush(Qt::NoBrush);
        painter->drawPath(path_);
    }
    painter->restore();
    if (const auto selection_pen = selection_frame_pen()) {
        painter->setBrush(Qt::NoBrush);
        painter->setPen(*selection_pen);
        painter->drawPath(path_);
    }
}
}
