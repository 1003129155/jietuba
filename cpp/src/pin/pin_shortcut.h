#pragma once
#include "core/shortcut_manager.h"
#include <QMouseEvent>
#include <QObject>
#include <QPointer>

namespace jietuba {
class PinShortcutController;
class PinWindow;

inline bool mouse_binding_matches(const QString &binding, QInputEvent *event, const QString &gesture) {
    return mouse_gesture_binding_matches(binding, event, gesture);
}

// Shared base of the pin handlers: they act on the target pin (PinShortcutController::find_target_pin).
class PinHandlerBase : public ShortcutHandler {
public:
    explicit PinHandlerBase(PinShortcutController *controller);
    // Rereads the bindings after a settings change.
    void reload_bindings();
    // Some pin could take the input; handle_key checks the target pin's editing state, as keys
    // and mouse buttons can target different pins.
    bool is_active() override;
    // The middle button runs through the same chain as keys.
    bool handle_mouse(QMouseEvent *event) override { return handle_key(event); }
    // Intercepts the smart translation hotkey (keyboard or side button) and translates
    // the target pin's selected OCR text directly. A pin under the pointer usually has no
    // keyboard focus, so the copy keystroke it injects would reach whatever window has
    // focus instead. Without a selection the normal copy detection runs.
    bool handle_hotkey(int hotkey_id, const std::function<void()> &callback) override;
    bool handle_mouse_hotkey(const QString &token, const std::function<void()> &callback) override;
    bool match(QInputEvent *event, const QString &cfg_key) const;
protected:
    PinWindow *find_target_pin(QInputEvent *event) const;
    // Keyboard focus is in a modal dialog (the pin's watermark text, toolbar layout): keys belong to the dialog,
    // even with the pointer on a pin.
    static bool typing_in_dialog(QInputEvent *event);
    // A popup menu does not take activation and the pointer stays on the pin: keys belong to the menu,
    // otherwise Esc closes the pin together with its context menu.
    static bool menu_open(QInputEvent *event);
    // Shortcuts that behave the same with and without editing.
    bool handle_shared(PinWindow *pin, QInputEvent *event);
private:
    QString selected_ocr_text() const;
    bool intercept_translation_hotkey(const std::function<void()> &callback);
    PinShortcutController *controller_;
    QHash<QString, KeyBinding> bindings_;
    QHash<QString, MouseBinding> mouse_bindings_;
};

// The target pin is being edited: copy, Esc leaves editing, delete. Priority 50.
class PinEditShortcutHandler : public PinHandlerBase {
public:
    using PinHandlerBase::PinHandlerBase;
    int priority() const override { return 50; }
    QString handler_name() const override { return "PinEdit"; }
    bool handle_key(QInputEvent *event) override;
};

// The target pin is not being edited: copy, thumbnail, lock, Esc closes. Priority 40.
class PinNormalShortcutHandler : public PinHandlerBase {
public:
    using PinHandlerBase::PinHandlerBase;
    int priority() const override { return 40; }
    QString handler_name() const override { return "PinNormal"; }
    bool handle_key(QInputEvent *event) override;
};

// Pin shortcuts: two handlers registered with the ShortcutManager, which find the target pin.
class PinShortcutController : public QObject {
    Q_OBJECT
public:
    static PinShortcutController *instance();
    // The controller if one was created, else nullptr.
    static PinShortcutController *existing();
    // Rereads both handlers' bindings after a settings change.
    void reload_bindings();
    void register_pin(PinWindow *pin_window);
    void unregister_pin(PinWindow *pin_window);
    // The topmost pin under the pointer.
    PinWindow *find_pin_under_cursor();
    // The pin whose window or toolbar is active, as its border shows.
    PinWindow *focused_pin();
    // Keys and hotkeys (event null) go to the focused pin, else to the pin under the pointer;
    // a mouse button goes to the pin it is pressed on.
    PinWindow *find_target_pin(QInputEvent *event);
    // Whether the event is the current binding of cfg_key.
    bool matches(QInputEvent *event, const QString &cfg_key) const;
private:
    PinShortcutController();
    QList<QPointer<PinWindow>> pin_windows_;
    std::unique_ptr<PinEditShortcutHandler> edit_handler_;
    std::unique_ptr<PinNormalShortcutHandler> normal_handler_;
};
}
