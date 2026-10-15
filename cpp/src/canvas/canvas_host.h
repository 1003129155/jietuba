#pragma once
#include <optional>

class QGraphicsScene;
class QMouseEvent;
class QWidget;

namespace jietuba {
class CanvasView;
class MagnifierOverlay;
class Toolbar;

// What a window hosting a CanvasView offers it (screenshot and pin windows).
// The reference looks these up by attribute name on view.window(); every
// member here corresponds to one such lookup and defaults to "absent".
class CanvasHost {
public:
    virtual ~CanvasHost() = default;
    // window.toolbar
    virtual Toolbar *canvas_toolbar() const { return nullptr; }
    // window.magnifier_overlay
    virtual MagnifierOverlay *canvas_magnifier_overlay() const { return nullptr; }
    // window._handle_capture_right_click
    virtual bool has_capture_right_click_handler() const { return false; }
    virtual void handle_capture_right_click(QMouseEvent *) {}
    // window._matches_capture_double_click; nullopt when the window has none.
    virtual std::optional<bool> matches_capture_double_click(QMouseEvent *) const { return std::nullopt; }
    // window._handle_double_click
    virtual bool has_double_click_handler() const { return false; }
    virtual bool handle_double_click(QMouseEvent *) { return false; }
    // window._handle_confirm
    virtual bool has_confirm_handler() const { return false; }
    virtual void handle_confirm() {}
    // window.view
    virtual CanvasView *canvas_view() const { return nullptr; }
    // window.update_toolbar_position; absent means nothing to do.
    virtual void update_toolbar_position() {}
};

CanvasHost *canvas_host_of(QWidget *widget);
// The host of the first view showing scene, as SmartEditController._panel() finds it.
CanvasHost *canvas_host_of_scene(QGraphicsScene *scene);
}
