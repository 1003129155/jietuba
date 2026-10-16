#pragma once
#include "canvas/items/drawing_items.h"
#include <QImage>

namespace jietuba {
// Paints a path with the background's reduced image as ink: a texture brush
// scaled up per block and pinned to the background's scene position. Without
// smoothing the result matches a full-resolution pixelated image pixel for
// pixel at 1/block_size^2 of the memory, and all strokes share the image.
class MosaicItem : public QGraphicsItem, public DrawingItemMixin {
public:
    MosaicItem(const QPainterPath &path, qreal brush_width, int block_size, const QImage &reduced_image,
               const QRectF &background_rect, bool fill_mode = false, bool smooth = false);
    bool fill_mode() const { return fill_mode_; }
    // Box mode is framed; freehand strokes are not, like pen strokes.
    bool shows_selection_frame() const override { return fill_mode_; }
    bool smooth() const { return smooth_; }
    // Mosaic vs blur is only the scaling filter; no new image is needed.
    void set_smooth(bool smooth);
    // The caller passes the background reduced with the new block size.
    void set_block_size(int block_size, const QImage &reduced_image);
    // Box mode only: the local rectangle used by the generic resize handles.
    QRectF rect() const { return rect_; }
    void setRect(const QRectF &rect);
    void set_path(const QPainterPath &path);
    QPainterPath path() const { return path_; }
    QPainterPath shape() const override;
    QRectF boundingRect() const override { return bounds_; }
    QImage reduced_image() const { return reduced_image_; }
    QRectF background_rect() const { return background_rect_; }
    // paint() pins the texture to scene coordinates, which are only valid in the
    // scene the item was created in; moving it to another scene (pin cloning)
    // must move the anchor too.
    void translate_background_anchor(qreal dx, qreal dy);
    qreal brush_width() const { return brush_width_; }
    int block_size() const { return block_size_; }
    std::optional<qreal> get_stroke_width() const override { return brush_width_; }
    void paint(QPainter *, const QStyleOptionGraphicsItem *, QWidget * = nullptr) override;
protected:
    void hoverEnterEvent(QGraphicsSceneHoverEvent *e) override { hover_enter(e); }
    void hoverMoveEvent(QGraphicsSceneHoverEvent *e) override { hover_move(e); }
    void hoverLeaveEvent(QGraphicsSceneHoverEvent *e) override { hover_leave(e); }
private:
    QPainterPath make_shape(const QPainterPath &path) const;
    QPainterPath path_;
    mutable std::optional<QPainterPath> shape_cache_;
    QRectF bounds_;
    qreal brush_width_;
    int block_size_;
    QImage reduced_image_;
    QRectF background_rect_;
    bool fill_mode_, smooth_;
    QRectF rect_;
};
}
