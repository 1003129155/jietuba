#include "canvas/items/mosaic_item.h"
#include "canvas/items/background_item.h"
#include "canvas/items/smart_erase_fill.h"
#include "canvas/scene.h"
#include <QGuiApplication>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPathStroker>
#include <algorithm>
#include <cmath>

namespace jietuba {
namespace {
// After a move or resize, waits for the edit to pause before rebuilding.
constexpr int SMART_ERASE_EDIT_DELAY_MS = 120;
// Shown where smart erase has no pixels yet; the selection frame's blue.
const QColor SMART_ERASE_PENDING(0, 180, 255, 50);
// A slow rebuild: a light band crosses the area once per period, fading in first.
constexpr qreal SWEEP_PERIOD_MS = 1200;
constexpr qreal SWEEP_FADE_MS = 150;

void paint_sweep(QPainter *painter, const QPainterPath &area, qint64 shown_ms) {
    const QRectF r = area.boundingRect();
    const qreal fade = std::min<qreal>(1.0, shown_ms / SWEEP_FADE_MS);
    const qreal phase = std::fmod(qreal(shown_ms), SWEEP_PERIOD_MS) / SWEEP_PERIOD_MS;
    const qreal band = std::clamp<qreal>(std::min(r.width(), r.height()) * 0.7, 40, 320);
    // The band leans, so it starts and ends this far beyond the sides.
    const qreal lean = r.height() * 0.35;
    const qreal x = r.left() - band - lean + phase * (r.width() + lean + 2 * band);
    QLinearGradient light(QPointF(x, r.top()), QPointF(x + band, r.top() + band * 0.35));
    light.setColorAt(0, QColor(255, 255, 255, 0));
    light.setColorAt(0.5, QColor(255, 255, 255, int(120 * fade)));
    light.setColorAt(1, QColor(255, 255, 255, 0));
    painter->setClipPath(area, Qt::IntersectClip);
    painter->fillRect(r, light);
}
}

MosaicItem::MosaicItem(const QPainterPath &path, qreal brush_width, int block_size, const QImage &reduced_image,
                       const QRectF &background_rect, bool fill_mode, MosaicKind kind)
    : QGraphicsItem(), DrawingItemMixin(this), brush_width_(std::max(1.0, brush_width)), block_size_(std::max(2, block_size)),
      reduced_image_(reduced_image), background_rect_(background_rect), fill_mode_(fill_mode), kind_(MosaicKind::Pixelate) {
    setZValue(5);
    // Smart erase follows moves and transforms.
    setFlag(ItemSendsGeometryChanges, true);
    // Selectable and movable like other strokes; box mode also gets the generic
    // eight-handle resize, which handle_editor drives through rect()/setRect().
    set_path(path);
    set_kind(kind);
}

MosaicItem::~MosaicItem() = default;

void MosaicItem::set_kind(MosaicKind kind) {
    if (kind == kind_) return;
    kind_ = kind;
    if (kind_ == MosaicKind::SmartErase) {
        fill_ = std::make_unique<SmartEraseFill>(
            [this]() -> std::optional<SmartEraseInput> {
                auto *canvas = dynamic_cast<CanvasScene *>(scene());
                if (!canvas || !canvas->background || path_.elementCount() == 0) return std::nullopt;
                // background_rect_ anchors the reduced image a pin inherits from the capture; the pixels
                // rebuilt from are this scene's own background.
                return SmartEraseInput{canvas->background->image(), canvas->scene_rect, sceneTransform().map(shape())};
            },
            [this]() { update(); });
        rebuild(false);
    } else {
        // The patch is only meaningful for smart erase; dropping it frees its pixels.
        fill_.reset();
    }
    update();
}

void MosaicItem::adopt_smart_erase(MosaicItem &source, const QPointF &offset) {
    // A stroke still being drawn has nothing to share; the copy rebuilds from its own scene.
    if (!fill_ || !source.fill_ || source.drawing_) return;
    pending_area_ = false;
    fill_->follow(*source.fill_, offset);
}

bool MosaicItem::smart_erase_in_progress() { return fill_ && !drawing_ && !fill_->ready() && fill_->start_now(); }

void MosaicItem::edit_finished() {
    if (fill_ && !drawing_ && !fill_->ready()) fill_->start_now();
}

void MosaicItem::set_drawing(bool drawing) {
    if (drawing == drawing_) return;
    drawing_ = drawing;
    geometry_changed();
}

void MosaicItem::geometry_changed() {
    if (!fill_) return;
    rebuild(drawing_ || QGuiApplication::mouseButtons() != Qt::NoButton || (pending_area_ && !fill_->ready()));
}

void MosaicItem::rebuild(bool shows_area) {
    pending_area_ = shows_area;
    if (drawing_) fill_->invalidate(-1);
    else fill_->invalidate(fill_->ready() ? SMART_ERASE_EDIT_DELAY_MS : 0);
}

QVariant MosaicItem::itemChange(GraphicsItemChange change, const QVariant &value) {
    if (fill_) {
        if (change == ItemPositionHasChanged || change == ItemTransformHasChanged) {
            geometry_changed();
        } else if (change == ItemSceneHasChanged) {
            // An item off the scene (undone, erased) lives on in the undo stack; it keeps only its
            // path and rebuilds the same pixels when it comes back.
            pending_area_ = drawing_;
            if (!scene()) fill_->invalidate(-1);
            else if (!fill_->ready() && !drawing_) fill_->invalidate(0);
        }
    }
    return QGraphicsItem::itemChange(change, value);
}

void MosaicItem::set_block_size(int block_size, const QImage &reduced_image) {
    block_size = std::max(2, block_size);
    if (block_size == block_size_) return;
    block_size_ = block_size;
    reduced_image_ = reduced_image;
    update();
}

void MosaicItem::setRect(const QRectF &rect) {
    // Undo commands set the rectangle a resize ended on; the same one must not drop the pixels.
    if (!fill_mode_ || rect == rect_) return;
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
    geometry_changed();
    update();
}

QPainterPath MosaicItem::shape() const {
    if (!shape_cache_) shape_cache_ = make_shape(path_);
    return *shape_cache_;
}

void MosaicItem::translate_background_anchor(qreal dx, qreal dy) {
    background_rect_.translate(dx, dy);
    geometry_changed();
    update();
}

void MosaicItem::paint(QPainter *painter, const QStyleOptionGraphicsItem *, QWidget *widget) {
    // elementCount, not isEmpty(): a lone moveTo is "empty" but must paint a dot.
    if (path_.elementCount() == 0) return;
    if (kind_ == MosaicKind::SmartErase) paint_smart_erase(painter, widget);
    else paint_texture(painter);
    if (const auto selection_pen = selection_frame_pen()) {
        painter->setBrush(Qt::NoBrush);
        painter->setPen(*selection_pen);
        painter->drawPath(path_);
    }
}

void MosaicItem::paint_texture(QPainter *painter) {
    if (reduced_image_.isNull()) return;
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
    painter->setRenderHint(QPainter::SmoothPixmapTransform, kind_ == MosaicKind::Blur);
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
}

void MosaicItem::paint_smart_erase(QPainter *painter, QWidget *widget) {
    if (!fill_) return;
    // scene.render() (exports, pin snapshots) passes no widget: the pixels must be final there.
    if (!widget && !drawing_ && !fill_->ready()) fill_->compute_now();
    const QPainterPath area = fill_mode_ ? path_ : shape();
    painter->save();
    if (!fill_->ready()) {
        const auto running = fill_->running_ms();
        const bool slow = !drawing_ && running && *running >= SmartEraseFill::SLOW_MS;
        if (drawing_ || pending_area_ || slow) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(SMART_ERASE_PENDING);
            painter->drawPath(area);
        }
        if (slow) paint_sweep(painter, area, *running - SmartEraseFill::SLOW_MS);
    } else if (const auto &patch = fill_->patch()) {
        bool invertible = false;
        const auto to_local = sceneTransform().inverted(&invertible);
        if (invertible) {
            painter->setClipPath(area, Qt::IntersectClip);
            // The patch lies in scene coordinates.
            painter->setTransform(to_local * painter->transform());
            painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
            painter->drawImage(patch->scene_rect, patch->image);
        }
    }
    painter->restore();
}
}
