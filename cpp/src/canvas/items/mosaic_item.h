#pragma once
#include "canvas/items/drawing_items.h"
#include <QImage>
#include <memory>

namespace jietuba {
class SmartEraseFill;

enum class MosaicKind { Pixelate, Blur, SmartErase };

// Paints a path with the background's reduced image as ink: a texture brush
// scaled up per block and pinned to the background's scene position. Without
// smoothing the result matches a full-resolution pixelated image pixel for
// pixel at 1/block_size^2 of the memory, and all strokes share the image.
// Smart erase instead keeps the background rebuilt under the shape, sized to the
// shape's bounds, and recomputes it whenever the shape moves.
class MosaicItem : public QGraphicsItem, public DrawingItemMixin {
public:
    MosaicItem(const QPainterPath &path, qreal brush_width, int block_size, const QImage &reduced_image,
               const QRectF &background_rect, bool fill_mode = false, MosaicKind kind = MosaicKind::Pixelate);
    ~MosaicItem() override;
    bool fill_mode() const { return fill_mode_; }
    // Box mode is framed; freehand strokes are not, like pen strokes.
    bool shows_selection_frame() const override { return fill_mode_; }
    MosaicKind kind() const { return kind_; }
    // Mosaic and blur differ only in the scaling filter; no new image is needed.
    void set_kind(MosaicKind kind);
    // While a stroke is drawn, smart erase only shows its area; the computation starts when it ends.
    void set_drawing(bool drawing);
    // A copy standing offset from the source in another scene (a pin) shows the source's pixels
    // rather than rebuilding them: the source saw the whole screenshot around the shape. A computation
    // the source has running is shared, not waited for.
    void adopt_smart_erase(MosaicItem &source, const QPointF &offset);
    const SmartEraseFill *smart_erase_fill() const { return fill_.get(); }
    // Smart erase only: whether its pixels are still to come. A rebuild waiting out the edit delay starts now.
    bool smart_erase_in_progress();
    // A move or resize has ended: the rebuild starts without the edit delay.
    void edit_finished();
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
    QVariant itemChange(GraphicsItemChange change, const QVariant &value) override;
    void hoverEnterEvent(QGraphicsSceneHoverEvent *e) override { hover_enter(e); }
    void hoverMoveEvent(QGraphicsSceneHoverEvent *e) override { hover_move(e); }
    void hoverLeaveEvent(QGraphicsSceneHoverEvent *e) override { hover_leave(e); }
private:
    QPainterPath make_shape(const QPainterPath &path) const;
    void paint_texture(QPainter *painter);
    void paint_smart_erase(QPainter *painter, QWidget *widget);
    // The shape or the background anchor moved.
    void geometry_changed();
    // Drops the patch; shows_area keeps the area marked until the new one is in.
    void rebuild(bool shows_area);
    QPainterPath path_;
    mutable std::optional<QPainterPath> shape_cache_;
    QRectF bounds_;
    qreal brush_width_;
    int block_size_;
    QImage reduced_image_;
    QRectF background_rect_;
    bool fill_mode_;
    MosaicKind kind_;
    bool drawing_ = false;
    // A shape drawn or dragged by the mouse stays marked until its pixels are in; other rebuilds show
    // the previous picture until they turn slow.
    bool pending_area_ = false;
    QRectF rect_;
    std::unique_ptr<SmartEraseFill> fill_;
};
}
