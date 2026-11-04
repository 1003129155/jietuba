#pragma once
#include "core/shortcut_manager.h"
#include <QHash>
#include <QMetaObject>
#include <QMouseEvent>
#include <QObject>
#include <memory>
#include <vector>

class QScreen;
class QWidget;

namespace jietuba {
class AnnotationToolbar;
class AnnotationView;
class CursorHighlight;
class ScreenAnnotation;

// Keys of the session while drawing: Esc returns to pass-through, undo and redo act on the layer, and
// the capture tool keys pick tools.
class ScreenAnnotationShortcutHandler : public ShortcutHandler {
public:
    explicit ScreenAnnotationShortcutHandler(ScreenAnnotation *owner);
    QString handler_name() const override { return "ScreenAnnotation"; }
    // The drawing layer, the toolbar and its floating panels.
    bool owns_window(const QWidget *window) const override;
    bool is_active() override;
    bool handle_mouse(QMouseEvent *event) override { return handle_key(event); }
    bool handle_key(QInputEvent *event) override;
private:
    bool match(QInputEvent *event, const QString &cfg_key) const;
    ScreenAnnotation *owner_;
    QHash<QString, KeyBinding> bindings_;
    QHash<QString, MouseBinding> mouse_bindings_;
    std::vector<std::pair<QString, QString>> tool_shortcuts_;
};

// The screen annotation session (#93): one drawing layer over every screen, a floating toolbar and the
// capture tools that need no captured image.
// PassThrough: the layer ignores the mouse (WS_EX_TRANSPARENT) and the focus stays where it was.
// Drawing: a tool is active and the layer takes the mouse; leaving gives the focus back to the window
// that had it, so keys such as a slide show's page keys keep working.
// Hidden: the layer is off the screen and keeps its drawings.
class ScreenAnnotation : public QObject {
    Q_OBJECT
public:
    enum class Mode { Off, PassThrough, Drawing, Hidden };
    Q_ENUM(Mode)
    explicit ScreenAnnotation(QObject *parent = nullptr);
    ~ScreenAnnotation() override;
    Mode mode() const { return mode_; }
    bool active() const { return mode_ != Mode::Off; }
    // The hotkey and tray entry: starts in pass-through, or ends the running session. Drawings go when
    // it ends, unless the keep setting is on; then they wait hidden for the next session.
    void toggle();
    void start();
    void stop();
    // "cursor" is pass-through; another tool starts drawing.
    void select_tool(const QString &tool_id);
    void set_drawings_visible(bool visible);
    // A light around the pointer and a ring for every click, in pass-through too.
    void set_cursor_highlight(bool enabled, bool remember = true);
    bool cursor_highlight_enabled() const;
    CursorHighlight *cursor_highlight() const { return highlight_; }
    // A white board over every screen, on and off with one click; turned on without a tool, it starts the
    // pen. It stays through pass-through and hiding, so the desktop does not show by accident, and lies
    // below the pins: pass-through moves them on the board, a tool draws over them.
    void toggle_board();
    bool board_on() const { return board_; }
    // One undo step that removes every drawing.
    void clear_drawings();
    void undo();
    void redo();
    bool can_undo() const;
    bool can_redo() const;
    // Before a capture: back to pass-through with the hit layer off the screen, so the capture window
    // is not covered and the captured pixels are not darkened by it. The drawings and the board are captured.
    void prepare_for_capture();
    AnnotationView *view() const { return view_; }
    AnnotationToolbar *toolbar() const { return toolbar_; }
    // Whether widget's window is the layer, the toolbar or one of its floating windows.
    bool owns_window(const QWidget *widget) const;
signals:
    void mode_changed(ScreenAnnotation::Mode mode);
private:
    // "Keep Screen Annotations": leaving hides the layer instead of removing it.
    bool keep_drawings() const;
    void discard_view();
    void create_view();
    // The layer follows the screens; connected for every screen of the session.
    void watch_screen(QScreen *screen);
    void fit_view();
    void connect_toolbar();
    void on_tool_changed(const QString &tool_id);
    void enter_drawing(const QString &tool_id);
    void enter_pass_through(bool restore_focus);
    void set_mode(Mode mode);
    void apply_board();
    // Puts the board right below this application's windows that are kept on top.
    void stack_board();
    void on_pin_created();
    void sync_effect_buttons();
    void sync_toolbar_style();
    void raise_toolbar();
    void remember_foreground();
    void restore_foreground();
    void on_application_state_changed(Qt::ApplicationState state);
    // Applies f to the layer when there is one, as the toolbar's style signals do.
    template <class F>
    void with_view(F &&f) {
        if (view_) f(view_);
    }

    Mode mode_ = Mode::Off;
    CursorHighlight *highlight_ = nullptr;
    bool board_ = false;
    AnnotationView *view_ = nullptr;
    AnnotationToolbar *toolbar_ = nullptr;
    std::unique_ptr<ScreenAnnotationShortcutHandler> shortcuts_;
    quintptr previous_foreground_ = 0;
    std::vector<QMetaObject::Connection> session_connections_;
};
}
