#pragma once
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <optional>

namespace jietuba {
class PinWindow;

// Thumbnail mode: entering, leaving and the view update. The window calls toggle()
// and, from resizeEvent, update_view().
class PinThumbnailMode {
public:
    explicit PinThumbnailMode(PinWindow *pin_window) : win_(pin_window) {}
    bool active() const { return active_; }
    int size() const { return size_; }
    void toggle();
    // Shows a region the user selected, without changing the source image.
    void enter_region(const QRectF &rect);
    // Shows the area around the scene centre point.
    void update_view();
private:
    void enter(const std::optional<QRectF> &region = std::nullopt);
    void exit();
    PinWindow *win_;
    bool active_ = false;
    int size_ = 100;
    std::optional<QRect> prev_geometry_;
    // Top-left of the thumbnail right after entering.
    QPoint entry_pos_;
    std::optional<QPointF> scene_center_;
    std::optional<QRectF> region_rect_;
};
}
