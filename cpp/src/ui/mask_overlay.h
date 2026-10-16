#pragma once
#include "canvas/view.h"
#include "core/hooks.h"
#include <QColor>
#include <QMetaObject>
#include <QPointer>
#include <QWidget>
#include <optional>

namespace jietuba {
class SelectionModel;

// Translucent mask outside the selection plus the full-screen crosshair that
// replaces the system cursor. A widget overlay over the capture window: it only
// repaints on rectChanged and stays out of scene.render() exports. Four fillRects
// instead of path subtraction; only the old/new selection difference is invalidated.
class MaskOverlayWidget : public CrosshairSurface {
    Q_OBJECT
public:
    MaskOverlayWidget(QWidget *parent, SelectionModel *selection_model);
    // One software cursor, painted in the same pass as the mask.
    void set_crosshair_position(const std::optional<QPoint> &position) override;
    std::optional<QPoint> crosshair_position() const { return crosshair_position_; }
    void set_mask_color(const QColor &color);
    QColor mask_color() const { return mask_color_; }
    // Scene coordinates to local integer pixels.
    QRect scene_to_local(const QRectF &sel) const;
    // Painted after the mask, each with its own painter (rounded corners, border/shadow preview).
    HookList<void(QPaintEvent *)> paint_hooks;
protected:
    void paintEvent(QPaintEvent *event) override;
private:
    void paint_mask();
    void paint_crosshair(QPainter &painter) const;
    void on_rect_changed(const QRectF &rect);
    QPointer<SelectionModel> model_;
    std::optional<QPoint> crosshair_position_;
    QColor mask_color_;
    QRect last_local_sel_;
};
}
