#pragma once
#include "core/shortcut_manager.h"
#include "ui/key_chip.h"
#include <QTimer>
#include <QWidget>
#include <memory>
#include <optional>
#include <vector>

class QHBoxLayout;

// Global hotkey editor with live detection of keys already taken system-wide, so a
// conflicting hotkey is never registered.
namespace jietuba {
class HotkeyLineEdit;

// The hotkey editor's handler, priority 200 (highest); registered while focused.
// handle_key keeps capture and pin handlers from taking the keys; handle_hotkey keeps
// registered global hotkeys from running the capture or clipboard callbacks;
// handle_captured_key takes the Win combinations and PrtSc the hook captured.
class HotkeyEditHandler : public ShortcutHandler {
public:
    explicit HotkeyEditHandler(HotkeyLineEdit *line_edit) : line_edit_(line_edit) {}
    int priority() const override { return 200; }
    QString handler_name() const override { return "HotkeyEditCapture"; }
    bool is_active() override { return active; }
    // Forwards the key to the editor, then swallows it from lower handlers.
    bool handle_key(QInputEvent *event) override;
    // A registered key never reaches Qt, so its registered key code is written in; an unknown
    // one is only intercepted.
    bool handle_hotkey(int hotkey_id, const std::function<void()> &callback) override;
    bool handle_captured_key(const QString &text) override;
    // Side buttons are swallowed whole by the hook while recording and never reach Qt,
    // so this is the only way to record them, wherever the pointer is. A side button is
    // atomic and never combined with modifiers, so it replaces the current content
    // (including an unfinished prefix such as "ctrl+").
    bool handle_mouse_hotkey(const QString &token, const std::function<void()> &callback) override;
    bool active = false;
private:
    HotkeyLineEdit *line_edit_;
};

// The key chip that captures hotkeys.
class HotkeyLineEdit : public KeyChipLineEdit {
    Q_OBJECT
public:
    explicit HotkeyLineEdit(QWidget *parent = nullptr);
    ~HotkeyLineEdit() override;
    // The key press entry shared with the handler.
    void capture_key(QKeyEvent *event);
protected:
    void keyPressEvent(QKeyEvent *event) override { capture_key(event); }
    void keyReleaseEvent(QKeyEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    // No context menu, so invalid text cannot be pasted.
    void contextMenuEvent(QContextMenuEvent *) override {}
private:
    void release_capture();
    std::unique_ptr<HotkeyEditHandler> shortcut_handler_;
    // Side-button exclusivity and key capture are reference counted; this keeps focus in / out paired.
    bool capture_held_ = false;
};

// Global hotkey editor: key chip plus a status icon (available / unset / conflict).
class HotkeyEdit : public QWidget {
    Q_OBJECT
public:
    explicit HotkeyEdit(QWidget *parent = nullptr);
    void setText(const QString &text);
    QString text() const;
    // "ok", "idle", "error", or empty when hidden.
    QString status_state() const { return status_icon->state(); }
    // A container-level error (for example two actions bound to one key).
    void set_validation_error(const QString &message = {});
    QString validation_error() const { return validation_error_; }
    // Synchronous final check before the settings window saves.
    bool validate_now();
    // Whether the value is taken over from Windows or another app that uses this key too.
    bool taken_over() const { return system_available_.value_or(false) && taken_over_; }
    HotkeyLineEdit *edit;
    StatusIcon *status_icon;
signals:
    void textChanged(const QString &text);
private:
    void on_text_changed(const QString &text);
    bool check_availability_now();
    // Container validation and the system check together; the container error wins.
    void render_status();
    std::optional<bool> system_available_;
    global_hotkey::Route route_ = global_hotkey::Route::Unset;
    bool taken_over_ = false;
    QString validation_error_;
    QHBoxLayout *layout_;
    HotkeySystem hotkey_system_;
    QTimer check_timer_;
};

// Validates editors as one conflict domain and writes the result back to each.
// One real key cannot serve two actions: the second registration would fail silently
// (RegisterHotKey refuses duplicates, the side-button table refuses them too). Empty
// values and unfinished prefixes are skipped. check_system also runs the system check,
// bypassing the 200 ms debounce before saving. Returns whether the whole group is usable.
bool validate_hotkey_group(const std::vector<HotkeyEdit *> &edits, bool check_system = false);
}
