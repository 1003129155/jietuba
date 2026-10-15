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

// Shared base of the pin handlers: they act on the pin under the pointer.
class PinHandlerBase : public ShortcutHandler {
public:
    explicit PinHandlerBase(PinShortcutController *controller);
    // Rereads the bindings after a settings change.
    void reload_bindings();
    // The middle button runs through the same chain as keys.
    bool handle_mouse(QMouseEvent *event) override { return handle_key(event); }
    // Intercepts the smart translation hotkey (keyboard or side button) and translates
    // the pin's selected OCR text directly. Pins are tool windows that never take
    // keyboard focus, so the copy keystroke it injects would reach whatever window has
    // focus instead. Without a selection the normal copy detection runs.
    bool handle_hotkey(int hotkey_id, const std::function<void()> &callback) override;
    bool handle_mouse_hotkey(const QString &token, const std::function<void()> &callback) override;
protected:
    PinWindow *find_pin_under_cursor() const;
    // Keyboard focus is in a modal dialog (the pin's watermark text, toolbar layout): keys belong to the dialog,
    // even with the pointer on a pin.
    static bool typing_in_dialog(QInputEvent *event);
    // A popup menu does not take activation and the pointer stays on the pin: keys belong to the menu,
    // otherwise Esc closes the pin together with its context menu.
    static bool menu_open(QInputEvent *event);
    bool match(QInputEvent *event, const QString &cfg_key) const;
    // Shortcuts that behave the same with and without editing.
    bool handle_shared(PinWindow *pin, QInputEvent *event);
private:
    QString selected_ocr_text() const;
    bool intercept_translation_hotkey(const std::function<void()> &callback);
    PinShortcutController *controller_;
    QHash<QString, KeyBinding> bindings_;
    QHash<QString, MouseBinding> mouse_bindings_;
};

// The pin under the pointer is being edited: copy, Esc leaves editing, delete. Priority 50.
class PinEditShortcutHandler : public PinHandlerBase {
public:
    using PinHandlerBase::PinHandlerBase;
    int priority() const override { return 50; }
    QString handler_name() const override { return "PinEdit"; }
    bool is_active() override;
    bool handle_key(QInputEvent *event) override;
};

// The pin under the pointer is not being edited: copy, thumbnail, lock, Esc closes. Priority 40.
class PinNormalShortcutHandler : public PinHandlerBase {
public:
    using PinHandlerBase::PinHandlerBase;
    int priority() const override { return 40; }
    QString handler_name() const override { return "PinNormal"; }
    bool is_active() override;
    bool handle_key(QInputEvent *event) override;
};

// "Press a shortcut over a pin": two handlers registered with the ShortcutManager,
// which find the pin under the pointer.
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
private:
    PinShortcutController();
    QList<QPointer<PinWindow>> pin_windows_;
    std::unique_ptr<PinEditShortcutHandler> edit_handler_;
    std::unique_ptr<PinNormalShortcutHandler> normal_handler_;
};
}
