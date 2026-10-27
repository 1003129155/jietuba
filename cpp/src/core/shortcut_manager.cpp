#include "core/shortcut_manager.h"
#include "core/input_hub.h"
#include "core/logger.h"
#include "settings/tool_settings.h"
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QGraphicsScene>
#include <QGraphicsTextItem>
#include <QGraphicsView>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <stdexcept>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace jietuba {
namespace {
// Input hub side-button names <-> tokens.
QString token_of_side_button(const QString &name) {
    if (name == "x1") return MOUSE_BUTTON_BACK;
    if (name == "x2") return MOUSE_BUTTON_FORWARD;
    return {};
}
QString side_button_of_token(const QString &token) { return token == MOUSE_BUTTON_BACK ? "x1" : "x2"; }

// "ctrl+shift+x" -> (modifier mask, remaining parts); shared by key and mouse parsing
// so "ctrl" means the same in both.
std::pair<Qt::KeyboardModifiers, QStringList> split_modifiers(const QString &text) {
    Qt::KeyboardModifiers mods = Qt::NoModifier;
    QStringList rest;
    for (const auto &raw : text.toLower().split('+')) {
        const auto part = raw.trimmed();
        if (part.isEmpty()) continue;
        if (part == "ctrl") mods |= Qt::ControlModifier;
        else if (part == "shift") mods |= Qt::ShiftModifier;
        else if (part == "alt") mods |= Qt::AltModifier;
        else rest.append(part);
    }
    return {mods, rest};
}

// Return and keypad Enter are both written "enter" in settings.
bool same_key(int pressed, int bound) {
    const auto is_enter = [](int key) { return key == Qt::Key_Return || key == Qt::Key_Enter; };
    return pressed == bound || (is_enter(pressed) && is_enter(bound));
}

const QStringList &default_inapp_keys() {
    // Shared by the key and mouse loaders, so a new entry applies to both tables.
    static const QStringList keys = {
        "inapp_confirm", "inapp_pin", "inapp_undo", "inapp_redo",
        "inapp_delete", "inapp_restore_last_region",
        "inapp_copy_pin", "inapp_copy_pin_text", "inapp_pin_reset_size",
        "inapp_thumbnail", "inapp_toggle_toolbar", "inapp_pin_lock", "inapp_pin_text_mode", "inapp_pin_text_selection", "inapp_pin_text_edit",
        "inapp_zoom_in", "inapp_zoom_out", "inapp_translate",
        "inapp_text_recognize",
    };
    return keys;
}

LogMsg handler_context(ShortcutHandler *handler, const char *method) { return LogMsg(QString("ShortcutManager: %1.%2").arg(handler->handler_name(), method)); }

QString hook_name(int hid) { return QString("hotkey-%1").arg(hid); }
}

QSet<std::pair<int, int>> ShortcutManager::registered_keys_global_;
QSet<QString> ShortcutManager::registered_mouse_buttons_global_;

Qt::KeyboardModifiers binding_modifiers(const QString &binding) { return split_modifiers(binding).first; }

bool is_mouse_button_hotkey(const QString &hotkey_str) { return global_hotkey::is_mouse_button(hotkey_str.trimmed().toLower()); }

bool is_inapp_mouse_shortcut(const QString &text) { return parse_inapp_mouse_to_qt(text).has_value(); }

bool mouse_gesture_binding_matches(const QString &binding, QInputEvent *event, const QString &gesture) {
    if (binding.isEmpty()) return false;
    const auto [modifiers, parts] = split_modifiers(binding);
    return parts == QStringList{gesture} && event->modifiers() == modifiers;
}

std::optional<std::pair<Qt::MouseButton, Qt::KeyboardModifiers>> parse_inapp_mouse_to_qt(const QString &text) {
    if (text.isEmpty()) return std::nullopt;
    const auto [mods, rest] = split_modifiers(text);
    if (rest.size() != 1 || rest.front() != MOUSE_BUTTON_MIDDLE) return std::nullopt;
    return std::make_pair(Qt::MiddleButton, mods);
}

QString inapp_shortcut_display_text(const QString &text) {
    if (text.isEmpty()) return {};
    const auto [mods, rest] = split_modifiers(text);
    if (rest.size() == 1 && rest.front() == MOUSE_BUTTON_MIDDLE) {
        const QString prefix = text.contains('+') ? text.left(text.lastIndexOf('+')).toUpper() + "+" : QString();
        // UI text through Qt translation, not the log translation T().
        return prefix + QCoreApplication::translate("InAppShortcut", "Middle");
    }
    return text.toUpper();
}

std::optional<QString> hotkey_identity(const QString &hotkey_str) {
    const auto normalized = hotkey_str.trimmed().toLower();
    if (normalized.isEmpty() || normalized.endsWith('+')) return std::nullopt;
    if (is_mouse_button_hotkey(normalized)) return "mouse:" + normalized;
    try {
        const auto [mods, vk] = ShortcutManager::parse_hotkey(normalized);
        return QString("keyboard:%1:%2").arg(mods).arg(vk);
    } catch (const std::invalid_argument &) {
        // Unparseable values still compare by text, so two identical invalid values
        // see each other; the editor reports unusable keys separately.
        return "invalid:" + normalized;
    }
}

bool ShortcutHandler::handle_mouse(QMouseEvent *) { return false; }
bool ShortcutHandler::handle_hotkey(int, const std::function<void()> &) { return false; }
bool ShortcutHandler::handle_mouse_hotkey(const QString &, const std::function<void()> &) { return false; }
bool ShortcutHandler::handle_captured_key(const QString &) { return false; }

bool HotkeyEventFilter::nativeEventFilter(const QByteArray &event_type, void *message, qintptr *result) {
    if (event_type != "windows_generic_MSG" && event_type != "windows_dispatcher_MSG") return false;
    const auto *msg = static_cast<const MSG *>(message);
    if (msg->message != WM_HOTKEY_MESSAGE) return false;
    if (manager_->on_wm_hotkey(static_cast<int>(msg->wParam))) {
        if (result) *result = 0;
        return true;
    }
    return false;
}

ShortcutManager::ShortcutManager() : native_filter_(this) {}

ShortcutManager *ShortcutManager::instance() {
    static ShortcutManager *manager = nullptr;
    if (!manager) {
        manager = new ShortcutManager;
        if (auto *app = QCoreApplication::instance()) {
            app->installEventFilter(manager);
            app->installNativeEventFilter(&manager->native_filter_);
            log_debug(T("ShortcutManager 已安装（KeyPress + WM_HOTKEY）"), "Shortcut");
        }
    }
    return manager;
}

void ShortcutManager::register_handler(ShortcutHandler *handler) {
    if (std::find(handlers_.begin(), handlers_.end(), handler) != handlers_.end()) return;
    handlers_.push_back(handler);
    std::stable_sort(handlers_.begin(), handlers_.end(), [](ShortcutHandler *a, ShortcutHandler *b) { return a->priority() > b->priority(); });
    log_debug(T("注册 handler: {handler_name} (优先级 {priority})，当前共 {handler_count} 个",
                {{"handler_name", handler->handler_name()}, {"priority", handler->priority()}, {"handler_count", static_cast<int>(handlers_.size())}}),
              "Shortcut");
}

void ShortcutManager::unregister_handler(ShortcutHandler *handler) {
    const auto it = std::find(handlers_.begin(), handlers_.end(), handler);
    if (it == handlers_.end()) return;
    handlers_.erase(it);
    log_debug(T("注销 handler: {handler_name}", {{"handler_name", handler->handler_name()}}), "Shortcut");
}

void ShortcutManager::set_global_hotkeys_suppressed(bool suppressed) { set_paused(suppressed, game_paused_); }

void ShortcutManager::set_game_mode_suppressed(bool suppressed) { set_paused(user_paused_, suppressed); }

void ShortcutManager::set_paused(bool user, bool game) {
    const bool was_paused = global_hotkeys_suppressed();
    user_paused_ = user;
    game_paused_ = game;
    const bool paused = global_hotkeys_suppressed();
    if (paused == was_paused) return;
    apply_registrations();
    sync_side_buttons();
    emit global_hotkeys_paused_changed(paused);
}

bool ShortcutManager::has_registered_hotkeys() const { return !registrations_.isEmpty() || !mouse_callbacks_.isEmpty(); }

bool ShortcutManager::is_text_input_active(QInputEvent *event) const {
    // Structural and function keys go to shortcuts even inside text fields.
    static const QSet<int> passthrough = {
        Qt::Key_Escape, Qt::Key_Tab, Qt::Key_Backtab, Qt::Key_F1, Qt::Key_F2, Qt::Key_F3, Qt::Key_F4,
        Qt::Key_F5, Qt::Key_F6, Qt::Key_F7, Qt::Key_F8, Qt::Key_F9, Qt::Key_F10, Qt::Key_F11, Qt::Key_F12,
    };
    if (passthrough.contains(event_key(event))) return false;
    auto *focus = QApplication::focusWidget();
    if (!focus) return false;
    if (qobject_cast<QLineEdit *>(focus) || qobject_cast<QTextEdit *>(focus) || qobject_cast<QPlainTextEdit *>(focus) || qobject_cast<QAbstractSpinBox *>(focus))
        return true;
    if (auto *combo = qobject_cast<QComboBox *>(focus); combo && combo->isEditable()) return true;
    // A text item being edited in a graphics view.
    if (auto *view = qobject_cast<QGraphicsView *>(focus)) {
        if (auto *scene = view->scene()) {
            auto *item = dynamic_cast<QGraphicsTextItem *>(scene->focusItem());
            if (item && item->hasFocus() && (item->textInteractionFlags() & Qt::TextEditorInteraction)) return true;
        }
    }
    return false;
}

bool ShortcutManager::filter_inapp_mouse(QObject *watched, QEvent *event) {
    // Every click in the application passes here, so non-middle buttons leave first.
    auto *mouse = static_cast<QMouseEvent *>(event);
    if (mouse->button() != Qt::MiddleButton) return false;
    // A hotkey editor must receive the press itself to record it; it may also sit
    // over a pin, whose handler checks only the pointer position, not focus.
    if (watched->property("_captures_inapp_mouse_shortcut").toBool()) return false;
    if (event->type() != QEvent::MouseButtonPress) return inapp_mouse_claimed_;
    inapp_mouse_claimed_ = dispatch_inapp_mouse(mouse);
    return inapp_mouse_claimed_;
}

bool ShortcutManager::dispatch_inapp_mouse(QMouseEvent *event) {
    const auto handlers = handlers_;
    for (auto *handler : handlers) {
        try {
            if (handler->is_active() && handler->handle_mouse(event)) {
                log_debug(T("鼠标键被 {handler_name} 消费 (button={button})", {{"handler_name", handler->handler_name()}, {"button", static_cast<int>(event->button())}}),
                          "Shortcut");
                return true;
            }
        } catch (const std::exception &e) {
            log_exception(e, handler_context(handler, "handle_mouse"));
        }
    }
    return false;
}

bool ShortcutManager::eventFilter(QObject *watched, QEvent *event) {
    // Installed on the application: only the type is checked here, everything else passes.
    const auto type = event->type();
    try {
        if (type == QEvent::KeyPress) return filter_key(static_cast<QInputEvent *>(event));
        if (type == QEvent::MouseButtonPress || type == QEvent::MouseButtonRelease || type == QEvent::MouseButtonDblClick)
            return filter_inapp_mouse(watched, event);
    } catch (const std::exception &e) {
        log_exception(e, LogMsg("ShortcutManager.eventFilter"));
    }
    return false;
}

bool ShortcutManager::filter_key(QInputEvent *event) {
    // A focused text input gets the key first.
    if (is_text_input_active(event)) return false;
    const auto handlers = handlers_;
    for (auto *handler : handlers) {
        try {
            if (handler->is_active() && handler->handle_key(event)) {
                log_debug(T("按键被 {handler_name} 消费 (key=0x{key_hex:X})", {{"handler_name", handler->handler_name()}, {"key_hex", event_key(event)}}), "Shortcut");
                return true;
            }
        } catch (const std::exception &e) {
            log_exception(e, handler_context(handler, "handle_key"));
        }
    }
    return false;
}

bool ShortcutManager::on_wm_hotkey(int hotkey_id) {
    const auto it = registrations_.constFind(hotkey_id);
    if (it == registrations_.constEnd()) return false;
    // A copy: the callback may re-register every hotkey.
    const auto callback = it->callback;
    // Messages posted before the pause still arrive; the handler chain gets no chance to intercept either.
    if (global_hotkeys_suppressed()) {
        log_debug(T("系统热键已临时禁用，忽略回调 (id={hotkey_id})", {{"hotkey_id", hotkey_id}}), "Shortcut");
        return true;
    }
    if (dispatch_hotkey(hotkey_id, callback)) return true;
    try {
        callback();
    } catch (const std::exception &e) {
        log_exception(e, T("热键回调 id={hotkey_id}", {{"hotkey_id", hotkey_id}}));
    }
    return true;
}

bool ShortcutManager::dispatch_hotkey(int hotkey_id, const std::function<void()> &callback) {
    const auto handlers = handlers_;
    for (auto *handler : handlers) {
        try {
            if (handler->is_active() && handler->handle_hotkey(hotkey_id, callback)) {
                log_debug(T("系统热键被 {handler_name} 拦截 (id={hotkey_id})", {{"handler_name", handler->handler_name()}, {"hotkey_id", hotkey_id}}), "Shortcut");
                return true;
            }
        } catch (const std::exception &e) {
            log_exception(e, handler_context(handler, "handle_hotkey"));
        }
    }
    return false;
}

bool ShortcutManager::dispatch_mouse_hotkey(const QString &token, const std::function<void()> &callback) {
    const auto handlers = handlers_;
    for (auto *handler : handlers) {
        try {
            if (handler->is_active() && handler->handle_mouse_hotkey(token, callback)) {
                log_debug(T("鼠标侧键热键被 {handler_name} 拦截 (token={token})", {{"handler_name", handler->handler_name()}, {"token", token}}), "Shortcut");
                return true;
            }
        } catch (const std::exception &e) {
            log_exception(e, handler_context(handler, "handle_mouse_hotkey"));
        }
    }
    return false;
}

void ShortcutManager::on_side_button(const QString &name) {
    const auto token = token_of_side_button(name);
    if (!token.isEmpty()) on_mouse_button_triggered(token);
}

void ShortcutManager::on_mouse_button_triggered(const QString &token) {
    // Paused, a recording hotkey editor still gets the button, without its callback.
    const bool paused = global_hotkeys_suppressed();
    if (paused && mouse_capture_refs_ == 0) {
        log_debug(T("系统热键已临时禁用，忽略鼠标侧键回调 (token={token})", {{"token", token}}), "Shortcut");
        return;
    }
    // The chain runs before the callback lookup, so hotkey editors can record unbound buttons.
    const auto callback = paused ? std::function<void()>() : mouse_callbacks_.value(token);
    if (dispatch_mouse_hotkey(token, callback)) return;
    if (!callback) return;
    try {
        callback();
    } catch (const std::exception &e) {
        log_exception(e, T("鼠标侧键热键回调 token={token}", {{"token", token}}));
    }
}

bool ShortcutManager::register_hotkey(const QString &hotkey_str, std::function<void()> callback) {
    const auto kind = global_hotkey::route(hotkey_str);
    if (kind == global_hotkey::Route::Mouse) return register_mouse_hotkey(hotkey_str.trimmed().toLower(), std::move(callback));
    if (kind != global_hotkey::Route::Register && kind != global_hotkey::Route::Hook) return false;
    const auto [mods, vk] = parse_hotkey(hotkey_str);
    // Refused here: with the hook as a fallback the system no longer refuses a repeat by itself,
    // and both callbacks would fire.
    if (registered_keys_global_.contains({mods, vk})) return false;
    const int hid = next_hotkey_id_++;
    registered_keys_global_.insert({mods, vk});
    registrations_.insert(hid, {mods, vk, kind, std::move(callback)});
    apply_registrations();
    if (global_hotkeys_suppressed()) return true;
    return registrations_.value(hid).registered || bound_hook_hotkeys_.contains(hook_name(hid));
}

void ShortcutManager::apply_registrations() {
    const bool paused = global_hotkeys_suppressed();
    for (auto it = registrations_.begin(); it != registrations_.end(); ++it) {
        if (paused && it->registered) {
            UnregisterHotKey(nullptr, it.key());
            it->registered = false;
        } else if (!paused && !it->registered) {
            it->registered = RegisterHotKey(nullptr, it.key(), static_cast<UINT>(it->mods), static_cast<UINT>(it->vk));
        }
    }
    sync_hook_hotkeys();
    if (paused) return;
    for (auto it = registrations_.begin(); it != registrations_.end(); ++it) {
        if (it->route != global_hotkey::Route::Register) continue;
        const bool hooked = !it->registered && bound_hook_hotkeys_.contains(hook_name(it.key()));
        if (hooked && !it->hooked) {
            const auto text = global_hotkey::format_hotkey(it->mods, it->vk).value_or(QString::number(it->vk));
            log_warning(T("全局热键被其他程序占用，改由钩子接管: {hotkey}", {{"hotkey", text}}), "Hotkey");
        }
        it->hooked = hooked;
    }
}

bool ShortcutManager::register_mouse_hotkey(const QString &token, std::function<void()> callback) {
    if (registered_mouse_buttons_global_.contains(token)) return false;
    mouse_callbacks_[token] = std::move(callback);
    if (!mouse_callback_order_.contains(token)) mouse_callback_order_.append(token);
    registered_mouse_buttons_global_.insert(token);
    sync_side_buttons();
    return true;
}

void ShortcutManager::begin_mouse_capture() {
    ++mouse_capture_refs_;
    sync_side_buttons();
}

void ShortcutManager::end_mouse_capture() {
    if (mouse_capture_refs_ <= 0) return;
    --mouse_capture_refs_;
    sync_side_buttons();
}

void ShortcutManager::sync_side_buttons() {
    // A focused hotkey editor records in this program's own window, so a pause leaves it alone.
    const bool enabled = (!mouse_callbacks_.isEmpty() && !global_hotkeys_suppressed()) || mouse_capture_refs_ > 0;
    try {
        // Nobody used side buttons yet: no hub is created just to disable them.
        auto *hub = enabled ? input_hub() : existing_input_hub();
        if (!hub) return;
        if (side_button_hub_ != hub) {
            connect(hub, &InputHub::side_button, this, &ShortcutManager::on_side_button, Qt::QueuedConnection);
            side_button_hub_ = hub;
        }
        QStringList suppressed;
        for (const auto &token : mouse_callback_order_) suppressed.append(side_button_of_token(token));
        hub->native->configure_side_buttons(enabled, suppressed, mouse_capture_refs_ > 0);
    } catch (const std::exception &e) {
        log_error(T("鼠标侧键监听设置失败: {e}", {{"e", QString::fromUtf8(e.what())}}), "Hotkey");
    }
}

void ShortcutManager::sync_hook_hotkeys() {
    // The hook swallows a key before the system sees it, so with both in place it fires once; the
    // registration of a hook-route key is a backstop for an elevated foreground window or a hook
    // the system removed. A register-route key goes to the hook only while the system refuses it.
    QMap<QString, std::pair<int, int>> wanted;
    if (!global_hotkeys_suppressed())
        for (auto it = registrations_.cbegin(); it != registrations_.cend(); ++it)
            if (it->route == global_hotkey::Route::Hook || !it->registered) wanted[hook_name(it.key())] = {it->mods, it->vk};
    try {
        auto *hub = !wanted.isEmpty() ? input_hub() : existing_input_hub();
        if (!hub) return;
        if (hook_hotkey_hub_ != hub) {
            connect(hub, &InputHub::hotkey, this, &ShortcutManager::on_hook_hotkey, Qt::QueuedConnection);
            hook_hotkey_hub_ = hub;
            bound_hook_hotkeys_.clear();
        }
        for (const auto &name : bound_hook_hotkeys_)
            if (!wanted.contains(name)) hub->native->unbind_hotkey(name);
        QSet<QString> bound;
        for (auto it = wanted.begin(); it != wanted.end(); ++it)
            if (hub->native->bind_hotkey(it.key(), global_hotkey::modifier_names(it->first), static_cast<quint32>(it->second)))
                bound.insert(it.key());
        bound_hook_hotkeys_ = bound;
    } catch (const std::exception &e) {
        log_error(T("系统组合键接管设置失败: {e}", {{"e", QString::fromUtf8(e.what())}}), "Hotkey");
    }
}

void ShortcutManager::on_hook_hotkey(const QString &name) {
    // Events queued before the pause still arrive.
    if (global_hotkeys_suppressed()) return;
    int hid = 0;
    for (auto it = registrations_.cbegin(); it != registrations_.cend() && !hid; ++it)
        if (hook_name(it.key()) == name) hid = it.key();
    if (!hid) return;
    // A copy: the callback may re-register every hotkey.
    const auto callback = registrations_.value(hid).callback;
    // Through the handler chain like WM_HOTKEY (a pin intercepts the translation hotkey, for one).
    if (dispatch_hotkey(hid, callback)) return;
    try {
        callback();
    } catch (const std::exception &e) {
        log_exception(e, T("系统组合键回调 name={name}", {{"name", name}}));
    }
}

void ShortcutManager::begin_key_capture() {
    ++key_capture_refs_;
    sync_key_capture();
}

void ShortcutManager::end_key_capture() {
    if (key_capture_refs_ <= 0) return;
    --key_capture_refs_;
    sync_key_capture();
}

void ShortcutManager::sync_key_capture() {
    const bool enabled = key_capture_refs_ > 0;
    try {
        auto *hub = enabled ? input_hub() : existing_input_hub();
        if (!hub) return;
        if (key_capture_hub_ != hub) {
            connect(hub, &InputHub::captured, this, &ShortcutManager::on_captured_key, Qt::QueuedConnection);
            key_capture_hub_ = hub;
        }
        hub->native->configure_key_capture(enabled);
    } catch (const std::exception &e) {
        log_error(T("录入按键截获设置失败: {e}", {{"e", QString::fromUtf8(e.what())}}), "Hotkey");
    }
}

void ShortcutManager::on_captured_key(const QStringList &modifiers, int vk) {
    const auto text = global_hotkey::format_hotkey(global_hotkey::modifier_mask(modifiers), vk);
    if (!text) return;
    const auto handlers = handlers_;
    for (auto *handler : handlers) {
        try {
            if (handler->is_active() && handler->handle_captured_key(*text)) return;
        } catch (const std::exception &e) {
            log_exception(e, handler_context(handler, "handle_captured_key"));
        }
    }
}

bool ShortcutManager::check_hotkey_availability(const QString &hotkey_str) {
    // Side buttons cannot be probed (RegisterHotKey takes no mouse buttons); duplicates
    // are caught on registration.
    if (is_mouse_button_hotkey(hotkey_str)) return true;
    std::pair<int, int> parsed;
    try {
        parsed = parse_hotkey(hotkey_str);
    } catch (const std::invalid_argument &) {
        return false;
    }
    try {
        const auto [mods, vk] = parsed;
        if (registered_keys_global_.contains({mods, vk})) return true;
        const int test_id = 9999;
        if (RegisterHotKey(nullptr, test_id, static_cast<UINT>(mods), static_cast<UINT>(vk))) {
            UnregisterHotKey(nullptr, test_id);
            return true;
        }
        return false;
    } catch (const std::exception &e) {
        log_exception(e, T("检查快捷键可用性"));
        return false;
    }
}

void ShortcutManager::unregister_all_hotkeys() {
    for (auto it = registrations_.cbegin(); it != registrations_.cend(); ++it) {
        if (it->registered) UnregisterHotKey(nullptr, it.key());
        registered_keys_global_.remove({it->mods, it->vk});
    }
    registrations_.clear();
    for (auto it = mouse_callbacks_.begin(); it != mouse_callbacks_.end(); ++it) registered_mouse_buttons_global_.remove(it.key());
    mouse_callbacks_.clear();
    mouse_callback_order_.clear();
    sync_side_buttons();
    sync_hook_hotkeys();
}

std::pair<int, int> ShortcutManager::parse_hotkey(const QString &hotkey) {
    const auto [mods, vk] = global_hotkey::parse_hotkey(hotkey);
    return {mods | MOD_NOREPEAT_FLAG, vk};
}

const QMap<int, QString> &get_key_display_map() {
    static const QMap<int, QString> map = [] {
        QMap<int, QString> result;
        const std::pair<int, const char *> entries[] = {
            {Qt::Key_Escape, "Esc"}, {Qt::Key_Tab, "Tab"}, {Qt::Key_Backtab, "Backtab"}, {Qt::Key_Backspace, "Backspace"},
            {Qt::Key_Return, "Enter"}, {Qt::Key_Enter, "Enter"}, {Qt::Key_Insert, "Insert"}, {Qt::Key_Delete, "Delete"},
            {Qt::Key_Pause, "Pause"}, {Qt::Key_Print, "Print"}, {Qt::Key_SysReq, "SysReq"}, {Qt::Key_Clear, "Clear"},
            {Qt::Key_Home, "Home"}, {Qt::Key_End, "End"}, {Qt::Key_Left, "Left"}, {Qt::Key_Up, "Up"},
            {Qt::Key_Right, "Right"}, {Qt::Key_Down, "Down"}, {Qt::Key_PageUp, "PageUp"}, {Qt::Key_PageDown, "PageDown"},
            {Qt::Key_Space, "Space"},
        };
        for (const auto &[key, name] : entries) result[key] = name;
        return result;
    }();
    return map;
}

const QMap<QString, int> &get_key_parse_map() {
    // Display names lowercased plus aliases; a later entry with the same name wins, as in the reference.
    static const QMap<QString, int> map = [] {
        QMap<QString, int> result;
        const std::tuple<const char *, int, std::vector<const char *>> entries[] = {
            {"Esc", Qt::Key_Escape, {"escape"}}, {"Tab", Qt::Key_Tab, {}}, {"Backtab", Qt::Key_Backtab, {}},
            {"Backspace", Qt::Key_Backspace, {}}, {"Enter", Qt::Key_Return, {"return"}}, {"Enter", Qt::Key_Enter, {}},
            {"Insert", Qt::Key_Insert, {}}, {"Delete", Qt::Key_Delete, {"del"}}, {"Pause", Qt::Key_Pause, {}},
            {"Print", Qt::Key_Print, {"printscreen", "prtsc"}}, {"SysReq", Qt::Key_SysReq, {}}, {"Clear", Qt::Key_Clear, {}},
            {"Home", Qt::Key_Home, {}}, {"End", Qt::Key_End, {}}, {"Left", Qt::Key_Left, {}}, {"Up", Qt::Key_Up, {}},
            {"Right", Qt::Key_Right, {}}, {"Down", Qt::Key_Down, {}}, {"PageUp", Qt::Key_PageUp, {}},
            {"PageDown", Qt::Key_PageDown, {}}, {"Space", Qt::Key_Space, {}},
        };
        for (const auto &[name, key, aliases] : entries) {
            result[QString(name).toLower()] = key;
            for (const auto *alias : aliases) result[QString(alias).toLower()] = key;
        }
        return result;
    }();
    return map;
}

std::optional<KeyBinding> parse_shortcut_to_qt(const QString &text) {
    if (text.isEmpty()) return std::nullopt;
    // Modifiers split like mouse bindings: one setting may hold either kind.
    const auto [mods, parts] = split_modifiers(text);
    if (parts.isEmpty()) return std::nullopt;
    const auto &key_map = get_key_parse_map();
    int key = Qt::Key_unknown;
    for (const auto &p : parts) {
        if (key_map.contains(p)) {
            key = key_map.value(p);
        } else if (p.size() == 1 && (p[0].isLetter() || p[0].isDigit())) {
            // Key_0..Key_9 equal '0'..'9', the same trick as letters.
            key = p.toUpper()[0].unicode();
        } else if (p.startsWith('f')) {
            bool ok = false;
            const int fn = p.mid(1).toInt(&ok);
            if (ok && p.size() > 1 && p[1].isDigit() && fn >= 1 && fn <= 24) key = Qt::Key_F1 + fn - 1;
        }
    }
    if (key == Qt::Key_unknown) return std::nullopt;
    return KeyBinding{key, mods};
}

bool is_reserved_inapp_shortcut(const QString &text) {
    const auto parsed = parse_shortcut_to_qt(text);
    return parsed && parsed->first == Qt::Key_Escape;
}

QHash<QString, KeyBinding> load_inapp_bindings(const QStringList &keys_of_interest) {
    auto *cfg = get_tool_settings_manager();
    const auto &keys = keys_of_interest.isEmpty() ? default_inapp_keys() : keys_of_interest;
    QHash<QString, KeyBinding> result;
    for (const auto &k : keys) {
        const auto text = cfg->get_inapp_shortcut(k);
        if (is_reserved_inapp_shortcut(text)) continue;
        if (const auto parsed = !text.isEmpty() ? parse_shortcut_to_qt(text) : std::nullopt) result[k] = *parsed;
    }
    return result;
}

QHash<QString, MouseBinding> load_inapp_mouse_bindings(const QStringList &keys_of_interest) {
    auto *cfg = get_tool_settings_manager();
    const auto &keys = keys_of_interest.isEmpty() ? default_inapp_keys() : keys_of_interest;
    QHash<QString, MouseBinding> result;
    for (const auto &k : keys)
        if (const auto parsed = parse_inapp_mouse_to_qt(cfg->get_inapp_shortcut(k))) result[k] = *parsed;
    return result;
}

int event_key(QInputEvent *event) {
    if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) return static_cast<QKeyEvent *>(event)->key();
    return Qt::Key_unknown;
}

bool event_is_auto_repeat(QInputEvent *event) {
    if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) return static_cast<QKeyEvent *>(event)->isAutoRepeat();
    return false;
}

bool is_mouse_shortcut_event(QInputEvent *event) { return dynamic_cast<QMouseEvent *>(event) != nullptr; }

bool match_inapp_binding(QInputEvent *event, const QString &cfg_key, const QHash<QString, KeyBinding> &key_bindings, const QHash<QString, MouseBinding> &mouse_bindings) {
    if (auto *mouse = dynamic_cast<QMouseEvent *>(event)) {
        if (!mouse_bindings.contains(cfg_key)) return false;
        const auto &binding = mouse_bindings[cfg_key];
        return mouse->button() == binding.first && mouse->modifiers() == binding.second;
    }
    if (!key_bindings.contains(cfg_key)) return false;
    const auto &binding = key_bindings[cfg_key];
    return same_key(event_key(event), binding.first) && (event->modifiers() & ~Qt::KeypadModifier) == binding.second;
}

QHash<int, std::pair<int, int>> load_move_keys() {
    const auto mode = get_tool_settings_manager()->get_inapp_cursor_move_mode();
    QHash<int, std::pair<int, int>> move;
    if (mode == "both" || mode == "wasd") {
        move[Qt::Key_W] = {0, -1};
        move[Qt::Key_S] = {0, 1};
        move[Qt::Key_A] = {-1, 0};
        move[Qt::Key_D] = {1, 0};
    }
    if (mode == "both" || mode == "arrows") {
        move[Qt::Key_Up] = {0, -1};
        move[Qt::Key_Down] = {0, 1};
        move[Qt::Key_Left] = {-1, 0};
        move[Qt::Key_Right] = {1, 0};
    }
    return move;
}
}
