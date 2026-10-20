#pragma once
#include "core/shortcut_manager.h"
#include <QHash>
#include <QMetaObject>
#include <QMouseEvent>
#include <QObject>
#include <QPointer>
#include <memory>
#include <vector>

class QScreen;
class QWidget;

namespace jietuba {
class AnnotationToolbar;
class AnnotationView;
class CursorHighlight;
class ScreenAnnotation;

// Keys of the session while drawing, priority 80 like the GIF drawing layer: Esc returns to
// pass-through, undo and redo run across the screens, and the capture tool keys pick tools.
class ScreenAnnotationShortcutHandler : public ShortcutHandler {
public:
    explicit ScreenAnnotationShortcutHandler(ScreenAnnotation *owner);
    int priority() const override { return 80; }
    QString handler_name() const override { return "ScreenAnnotation"; }
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

// The screen annotation session (#93): a drawing layer on every screen, a floating toolbar and the
// capture tools that need no captured image.
// PassThrough: the layers ignore the mouse (WS_EX_TRANSPARENT) and the focus stays where it was.
// Drawing: a tool is active and the layers take the mouse; leaving gives the focus back to the window
// that had it, so keys such as a slide show's page keys keep working.
// Hidden: the layers are off the screen and keep their drawings.
// Each layer keeps its own undo stack; history_ records which layers each step changed, so undo and
// redo follow the drawing order across screens.
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
    // "cursor" is pass-through; another tool starts drawing on every screen.
    void select_tool(const QString &tool_id);
    void set_drawings_visible(bool visible);
    // A light around the pointer and a ring for every click, in pass-through too.
    void set_cursor_highlight(bool enabled, bool remember = true);
    bool cursor_highlight_enabled() const;
    CursorHighlight *cursor_highlight() const { return highlight_; }
    // A white board over every screen, on and off with one click. It needs a tool, so it starts the pen
    // when the layers pass the mouse through. Pass-through shows the desktop again; the board returns
    // with the next tool.
    void toggle_board();
    bool board_on() const { return board_; }
    // One undo step that removes every drawing on every screen.
    void clear_drawings();
    void undo();
    void redo();
    bool can_undo() const { return !history_.empty(); }
    bool can_redo() const { return !redo_.empty(); }
    // Before a capture: back to pass-through with the hit layer off the screen, so the capture window
    // is not covered and the captured pixels are not darkened by it. The drawings are captured.
    void prepare_for_capture();
    const std::vector<AnnotationView *> &views() const { return views_; }
    AnnotationToolbar *toolbar() const { return toolbar_; }
    // The layer drawn on last (keys, toolbar syncing); the one under the pointer before that.
    AnnotationView *active_view() const;
    // Whether widget's window is a layer, the toolbar or one of its floating windows.
    bool owns_window(QWidget *widget) const;
signals:
    void mode_changed(ScreenAnnotation::Mode mode);
private:
    // "Keep Screen Annotations": leaving hides the layers instead of removing them.
    bool keep_drawings() const;
    void discard_views();
    void add_view(QScreen *screen);
    void remove_view(AnnotationView *view);
    void connect_toolbar();
    void on_tool_changed(const QString &tool_id);
    void enter_drawing(const QString &tool_id);
    void enter_pass_through(bool restore_focus);
    void set_mode(Mode mode);
    void apply_board();
    void sync_effect_buttons();
    void sync_toolbar_style();
    void raise_toolbar();
    void remember_foreground();
    void restore_foreground();
    void on_application_state_changed(Qt::ApplicationState state);
    void on_stack_index_changed(AnnotationView *view, int index);
    AnnotationView *view_under_cursor() const;
    // Applies f to every layer, as the toolbar's style signals do.
    template <class F>
    void for_each_view(F &&f) {
        for (auto *view : views_) f(view);
    }

    Mode mode_ = Mode::Off;
    CursorHighlight *highlight_ = nullptr;
    bool board_ = false;
    std::vector<AnnotationView *> views_;
    AnnotationToolbar *toolbar_ = nullptr;
    QPointer<AnnotationView> active_view_;
    std::unique_ptr<ScreenAnnotationShortcutHandler> shortcuts_;
    quintptr previous_foreground_ = 0;
    std::vector<std::vector<AnnotationView *>> history_, redo_;
    QHash<AnnotationView *, int> stack_index_;
    // Set while the session itself moves a stack, so the index signals are not taken as new steps.
    bool coordinating_ = false;
    std::vector<QMetaObject::Connection> session_connections_;
};
}
