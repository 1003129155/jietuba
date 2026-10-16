#pragma once
#include "core/global_hotkey.h"
#include <QAbstractNativeEventFilter>
#include <QHash>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

class QInputEvent;
class QMouseEvent;

namespace jietuba {
class InputHub;

// Unified shortcut handling, merging four mechanisms:
//   1. System-wide hotkeys (RegisterHotKey / WM_HOTKEY)
//   2. Mouse side-button hotkeys (the input hub's native hooks)
//   3. Win combinations and PrtSc (taken over by the input hub, system registration as a backstop)
//   4. In-app Qt key press dispatch
// global_hotkey::route decides the path of a global hotkey. All four share one
// priority-ordered handler chain, resolving conflicts between modules.
// Priorities (higher first): 200 hotkey editor, 100 capture, 80 GIF drawing,
// 60 clipboard window, 50 pin editing, 40 pin hover.

constexpr unsigned WM_HOTKEY_MESSAGE = 0x0312;
constexpr int MOD_ALT_FLAG = global_hotkey::MOD_ALT;
constexpr int MOD_CONTROL_FLAG = global_hotkey::MOD_CONTROL;
constexpr int MOD_SHIFT_FLAG = global_hotkey::MOD_SHIFT;
constexpr int MOD_WIN_FLAG = global_hotkey::MOD_WIN;
constexpr int MOD_NOREPEAT_FLAG = global_hotkey::MOD_NOREPEAT;

// RegisterHotKey only fires for keyboard keys, so side buttons (MOUSE_BUTTON_BACK /
// MOUSE_BUTTON_FORWARD) dispatch globally through the input hub's hook.
bool is_mouse_button_hotkey(const QString &hotkey_str);

// The middle button is an in-app shortcut only and deliberately not a global token:
// a global one would hook and swallow it everywhere, breaking paste, closing tabs
// and autoscroll in every program. In-app dispatch needs no hook.
inline const QString MOUSE_BUTTON_MIDDLE = "mousemiddle";
// The modifiers a binding names (ctrl/shift/alt), shared by key and mouse parsing.
Qt::KeyboardModifiers binding_modifiers(const QString &binding);
// Whether text is an in-app mouse binding, optionally with modifiers ("ctrl+mousemiddle").
bool is_inapp_mouse_shortcut(const QString &text);
// Whether an action-oriented mouse binding matches this event's modifiers and gesture.
bool mouse_gesture_binding_matches(const QString &binding, QInputEvent *event, const QString &gesture);
// "mousemiddle" / "ctrl+mousemiddle" -> (button, modifiers); nullopt when not a mouse
// binding, so callers fall back to keyboard parsing. Never throws: both kinds share a setting.
std::optional<std::pair<Qt::MouseButton, Qt::KeyboardModifiers>> parse_inapp_mouse_to_qt(const QString &text);
// Setting value -> menu text; the middle button gets a short label.
QString inapp_shortcut_display_text(const QString &text);
// A comparable identity for a hotkey string, independent of case, spaces and modifier
// order. nullopt means unbound: empty values and unfinished prefixes such as "ctrl+"
// never conflict with one another.
std::optional<QString> hotkey_identity(const QString &hotkey_str);

// Receives selected text directly instead of probing the system for a copy.
class SelectionTranslator {
public:
    virtual ~SelectionTranslator() = default;
    virtual void translate_selection(const QString &text) = 0;
};
// The callback type of the smart translation hotkey, so handlers can recognize it and
// hand over a selection they own (the reference checks the bound method's owner).
struct SelectionTranslationHotkey {
    SelectionTranslator *owner;
    std::function<void()> trigger;
    void operator()() const { trigger(); }
};

// A module's shortcut handler, registered with the manager.
class ShortcutHandler {
public:
    virtual ~ShortcutHandler() = default;
    // Whether this handler should receive keys now.
    virtual bool is_active() = 0;
    // Handles a key press (and, through handle_mouse, middle-button presses); true when consumed.
    virtual bool handle_key(QInputEvent *event) = 0;
    // In-app mouse button presses (middle only), same meaning as handle_key. Most
    // implementations return handle_key(event): bindings matched through
    // match_inapp_binding work for both, and key-only branches see Key_unknown.
    virtual bool handle_mouse(QMouseEvent *event);
    // Asked before a WM_HOTKEY callback runs; true intercepts it.
    virtual bool handle_hotkey(int hotkey_id, const std::function<void()> &callback);
    // Asked before a side-button callback runs; callback may be empty when the button is
    // unbound, so hotkey editors can still record it. True intercepts it.
    virtual bool handle_mouse_hotkey(const QString &token, const std::function<void()> &callback);
    // A key the hook took while a hotkey editor has focus: Win combinations and PrtSc never
    // reach Qt. text is the recorder's spelling, such as "shift+win+s".
    virtual bool handle_captured_key(const QString &text);
    // Higher first.
    virtual int priority() const = 0;
    virtual QString handler_name() const = 0;
};

class ShortcutManager;

// Intercepts WM_HOTKEY and lets the manager dispatch it.
class HotkeyEventFilter : public QAbstractNativeEventFilter {
public:
    explicit HotkeyEventFilter(ShortcutManager *manager) : manager_(manager) {}
    bool nativeEventFilter(const QByteArray &event_type, void *message, qintptr *result) override;
private:
    ShortcutManager *manager_;
};

// Singleton installed on the application: RegisterHotKey hotkeys, side-button hotkeys
// and in-app key presses.
class ShortcutManager : public QObject {
    Q_OBJECT
public:
    static ShortcutManager *instance();
    void register_handler(ShortcutHandler *handler);
    void unregister_handler(ShortcutHandler *handler);
    // Callbacks swallowed temporarily while the Windows registrations stay.
    bool global_hotkeys_suppressed() const { return global_hotkeys_suppressed_; }
    // Taken-over system combinations go back to the system while suppressed.
    void set_global_hotkeys_suppressed(bool suppressed);
    bool has_registered_hotkeys() const;
    // A global hotkey; global_hotkey::route decides system registration, the hook or side buttons.
    bool register_hotkey(const QString &hotkey_str, std::function<void()> callback);
    // Hotkey editor focused: takes all side buttons so unbound ones can be recorded.
    void begin_mouse_capture();
    // Hotkey editor unfocused: unbound side buttons go back to other programs.
    void end_mouse_capture();
    // Hotkey editor focused: the hook takes Win combinations and PrtSc.
    void begin_key_capture();
    // Hotkey editor unfocused: they go back to the system.
    void end_key_capture();
    // Whether the system registration would succeed, by registering it temporarily. Only
    // that layer: a hook-route key works even when it fails (see global_hotkey::route).
    bool check_hotkey_availability(const QString &hotkey_str);
    void unregister_all_hotkeys();
    // "ctrl+shift+a" -> (modifiers | MOD_NOREPEAT, vk); throws std::invalid_argument.
    static std::pair<int, int> parse_hotkey(const QString &hotkey);
    // (Windows modifier bits, virtual key) of a registered hotkey id.
    std::optional<std::pair<int, int>> hotkey_metadata(int hotkey_id) const {
        const auto it = id_to_metadata_.find(hotkey_id);
        return it == id_to_metadata_.end() ? std::nullopt : std::optional<std::pair<int, int>>(*it);
    }
    // The WM_HOTKEY entry point; returns whether the message was handled.
    bool on_wm_hotkey(int hotkey_id);
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    ShortcutManager();
    bool is_text_input_active(QInputEvent *event) const;
    bool filter_inapp_mouse(QObject *watched, QEvent *event);
    bool dispatch_inapp_mouse(QMouseEvent *event);
    bool filter_key(QInputEvent *event);
    bool dispatch_hotkey(int hotkey_id, const std::function<void()> &callback);
    bool dispatch_mouse_hotkey(const QString &token, const std::function<void()> &callback);
    void on_side_button(const QString &name);
    // Main-thread entry for side-button presses, queued from the input hub.
    void on_mouse_button_triggered(const QString &token);
    // Same-process dedupe, matching RegisterHotKey refusing duplicates.
    bool register_mouse_hotkey(const QString &token, std::function<void()> callback);
    bool register_system_hotkey(int hid, int mods, int vk, const std::function<void()> &callback);
    // The only place side-button reporting and swallowing change; idempotent. The hook
    // both swallows bound buttons from other programs and, while an editor has focus,
    // takes all of them; with neither it must stop, or unbound buttons stay taken.
    void sync_side_buttons();
    // Binds and unbinds per registrations and suppression; idempotent. Swallowing a
    // system combination without responding disables it, so suppression unbinds.
    void sync_hook_hotkeys();
    void on_hook_hotkey(const QString &name);
    void sync_key_capture();
    void on_captured_key(const QStringList &modifiers, int vk);

    std::vector<ShortcutHandler *> handlers_;
    QHash<int, std::function<void()>> id_to_callback_;
    QHash<int, std::pair<int, int>> id_to_metadata_;
    int next_hotkey_id_ = 1;
    bool global_hotkeys_suppressed_ = false;
    QHash<QString, std::function<void()>> mouse_callbacks_;
    QStringList mouse_callback_order_;
    int mouse_capture_refs_ = 0;
    InputHub *side_button_hub_ = nullptr;
    // Bound by name; the hotkey id is shared with the backstop registration.
    struct HookHotkey {
        int hid;
        int mods;
        int vk;
        std::function<void()> callback;
    };
    QMap<QString, HookHotkey> hook_hotkeys_;
    QSet<QString> bound_hook_hotkeys_;
    InputHub *hook_hotkey_hub_ = nullptr;
    int key_capture_refs_ = 0;  // focused hotkey editors
    InputHub *key_capture_hub_ = nullptr;
    // Set when a press is consumed, so its release and double-click are swallowed too.
    bool inapp_mouse_claimed_ = false;
    HotkeyEventFilter native_filter_;
    static QSet<std::pair<int, int>> registered_keys_global_;
    static QSet<QString> registered_mouse_buttons_global_;
};

// Public hotkey registration, delegating to the manager.
class HotkeySystem {
public:
    HotkeySystem() : mgr_(ShortcutManager::instance()) {}
    bool register_hotkey(const QString &hotkey_str, std::function<void()> callback) { return mgr_->register_hotkey(hotkey_str, std::move(callback)); }
    bool check_hotkey_availability(const QString &hotkey_str) { return mgr_->check_hotkey_availability(hotkey_str); }
    void unregister_all() { mgr_->unregister_all_hotkeys(); }
    void set_suppressed(bool suppressed) { mgr_->set_global_hotkeys_suppressed(suppressed); }
    bool has_registered_hotkeys() const { return mgr_->has_registered_hotkeys(); }
private:
    ShortcutManager *mgr_;
};

// The key name tables, shared by the hotkey editors and parse_shortcut_to_qt.
const QMap<int, QString> &get_key_display_map();
const QMap<QString, int> &get_key_parse_map();
using KeyBinding = std::pair<int, Qt::KeyboardModifiers>;
using MouseBinding = std::pair<Qt::MouseButton, Qt::KeyboardModifiers>;
// "ctrl+c" / "pageup" / "shift+c" -> (Qt key, modifiers); nullopt when it cannot be parsed.
std::optional<KeyBinding> parse_shortcut_to_qt(const QString &text);
// In-app bindings on a fixed key (Esc) that cannot be overridden.
bool is_reserved_inapp_shortcut(const QString &text);
// {config key: key binding}; mouse bindings are skipped (see load_inapp_mouse_bindings).
QHash<QString, KeyBinding> load_inapp_bindings(const QStringList &keys_of_interest = {});
// {config key: mouse binding}, from the same settings; a setting is one kind or the other.
QHash<QString, MouseBinding> load_inapp_mouse_bindings(const QStringList &keys_of_interest = {});
// The event's key; Key_unknown for mouse events, so key-only branches skip them.
int event_key(QInputEvent *event);
// Keyboard auto-repeat; mouse events never repeat.
bool event_is_auto_repeat(QInputEvent *event);
bool is_mouse_shortcut_event(QInputEvent *event);
// Whether the event matches a setting's binding, looked up by event type.
bool match_inapp_binding(QInputEvent *event, const QString &cfg_key, const QHash<QString, KeyBinding> &key_bindings,
                         const QHash<QString, MouseBinding> &mouse_bindings = {});
// {Qt key: (dx, dy)} per the cursor nudge setting.
QHash<int, std::pair<int, int>> load_move_keys();
}
