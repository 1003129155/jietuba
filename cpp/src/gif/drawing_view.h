#pragma once
#include "canvas/scene.h"
#include "canvas/view.h"
#include "core/shortcut_manager.h"
#include <QMouseEvent>
#include <QRect>
#include <memory>

class QGraphicsRectItem;

// The GIF recording drawing layer, reusing the screenshot CanvasView and SmartEditController.
// Recording grabs screen pixels, so whatever this window draws is recorded as well.
// The window is a frameless, transparent top-level window over the recording area.
// The scene is a CanvasScene with its background and selection item hidden and an empty
// selection already confirmed, so the view goes straight to editing; the view only adds
// click-through (WS_EX_TRANSPARENT) on and off.
namespace jietuba {
class GifDrawingView;

// Set on an item the drawing layer shows but does not count as an annotation: it is neither cleared
// nor kept.
constexpr int TRANSIENT_ITEM_DATA = 0x7a11;

class GifDrawingScene : public CanvasScene {
    Q_OBJECT
public:
    GifDrawingScene(int width, int height);
    void resize_scene(int width, int height);
    void set_hit_test_visible(bool visible);
    // Annotations only: the hit rectangle, background and selection item are excluded.
    bool is_drawing(QGraphicsItem *item) const;
    // Mouse capture background: alpha 1 is invisible but lets Windows hit-test the pixels.
    QGraphicsRectItem *hit_rect = nullptr;
};

// Shortcuts of the GIF drawing layer, priority 80.
class GifDrawingShortcutHandler : public ShortcutHandler {
public:
    explicit GifDrawingShortcutHandler(GifDrawingView *view);
    int priority() const override { return 80; }
    QString handler_name() const override { return "GifDrawing"; }
    bool is_active() override;
    // The middle button runs through the same chain as keys.
    bool handle_mouse(QMouseEvent *event) override { return handle_key(event); }
    bool handle_key(QInputEvent *event) override;
private:
    bool match(QInputEvent *event, const QString &cfg_key) const;
    GifDrawingView *view_;
    QHash<QString, KeyBinding> bindings_;
    QHash<QString, MouseBinding> mouse_bindings_;
};

// Pass-through (cursor tool): WS_EX_TRANSPARENT, the mouse reaches the windows below.
// Drawing (other tools): pass-through off, CanvasView takes the mouse.
class GifDrawingView : public CanvasView {
    Q_OBJECT
public:
    // Owns its scene. Without own_shortcuts the owner handles the keys, as the screen annotation
    // does across its per-screen layers.
    explicit GifDrawingView(const QRect &rect, bool own_shortcuts = true);
    ~GifDrawingView() override;
    GifDrawingScene *scene_obj() const { return gif_scene_; }
    CommandUndoStack *undo_stack() const;
    bool passthrough() const { return passthrough_; }
    // Follows the recording area's position and size.
    void update_rect(const QRect &rect);
    void activate_tool(const QString &tool_id);
    void set_passthrough(bool enable);
    // Clears every drawing (the hit rectangle stays).
    void clear_all();
    bool has_drawings() const;
signals:
    void tool_changed(const QString &tool_id);
    void passthrough_changed(bool enabled);
protected:
    // Pass-through blocks every mouse event from CanvasView so windows below are not disturbed.
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    // Shortcuts go through ShortcutManager; this only passes keys through.
    void keyPressEvent(QKeyEvent *event) override;
private:
    void apply_passthrough(bool enable);
    void on_gif_tool_changed(const QString &tool_id);
    GifDrawingScene *gif_scene_;
    bool passthrough_ = true;
    std::unique_ptr<GifDrawingShortcutHandler> shortcut_handler_;
};
}
