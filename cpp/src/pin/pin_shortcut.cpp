#include "pin/pin_shortcut.h"
#include "core/logger.h"
#include "pin/pin_canvas.h"
#include "pin/pin_canvas_view.h"
#include "pin/pin_manager.h"
#include "pin/pin_text_mode.h"
#include "pin/pin_toolbar.h"
#include "pin/pin_window.h"
#include "canvas/smart_edit_controller.h"
#include <QApplication>
#include <QCursor>

namespace jietuba {
namespace {
const QStringList PIN_KEYS = {
    "inapp_copy_pin", "inapp_copy_pin_text", "inapp_pin_reset_size",
    "inapp_thumbnail", "inapp_toggle_toolbar", "inapp_delete", "inapp_pin_lock", "inapp_pin_text_mode",
    "inapp_pin_text_selection", "inapp_pin_text_edit",
};

// Esc clears a text selection before it leaves a tool or closes the pin.
bool clear_text_selection(PinWindow *pin) { return pin->text_selection() && pin->text_selection()->clear_selection(); }
}

PinHandlerBase::PinHandlerBase(PinShortcutController *controller) : controller_(controller) { reload_bindings(); }

void PinHandlerBase::reload_bindings() {
    bindings_ = load_inapp_bindings(PIN_KEYS);
    mouse_bindings_ = load_inapp_mouse_bindings(PIN_KEYS);
    // Mouse reset belongs to the dedicated mouse settings (including migration).
    mouse_bindings_.remove("inapp_pin_reset_size");
}

PinWindow *PinHandlerBase::find_target_pin(QInputEvent *event) const { return controller_->find_target_pin(event); }

bool PinHandlerBase::owns_window(const QWidget *window) const { return controller_->pin_of_window(window) != nullptr; }

bool PinHandlerBase::typing_in_dialog(QInputEvent *event) {
    if (event->type() != QEvent::KeyPress) return false;
    auto *window = QApplication::activeWindow();
    return window && window->isModal();
}

bool PinHandlerBase::menu_open(QInputEvent *event) { return event->type() == QEvent::KeyPress && QApplication::activePopupWidget(); }

bool PinHandlerBase::match(QInputEvent *event, const QString &cfg_key) const { return match_inapp_binding(event, cfg_key, bindings_, mouse_bindings_); }

bool PinHandlerBase::handle_shared(PinWindow *pin, QInputEvent *event) {
    if (match(event, "inapp_copy_pin")) {
        // In text mode the image's text layer is hidden; the pin copies the text's own selection.
        auto *selection = pin->text_mode() ? nullptr : pin->text_selection();
        if (!(selection && selection->copy_selected_text())) pin->copy_to_clipboard();
        return true;
    }
    if (match(event, "inapp_copy_pin_text")) {
        pin->copy_all_text();
        return true;
    }
    if (match(event, "inapp_toggle_toolbar")) {
        pin->toggle_toolbar();
        return true;
    }
    if (match(event, "inapp_pin_reset_size")) {
        // The size belongs to the thumbnail or the text there, as in the context menu.
        if (pin->thumbnail_mode() || pin->text_mode()) return false;
        if (pin->lock_blocks("move")) pin->show_locked_hint();
        else pin->reset_to_original_size();
        return true;
    }
    return false;
}

QString PinHandlerBase::selected_ocr_text() const {
    auto *pin = find_target_pin(nullptr);
    if (!pin || !pin->text_selection()) return {};
    return pin->text_selection()->selected_text().trimmed();
}

bool PinHandlerBase::handle_hotkey(int, const std::function<void()> &callback) { return intercept_translation_hotkey(callback); }

bool PinHandlerBase::handle_mouse_hotkey(const QString &, const std::function<void()> &callback) { return intercept_translation_hotkey(callback); }

bool PinHandlerBase::intercept_translation_hotkey(const std::function<void()> &callback) {
    const auto *hotkey = callback ? callback.target<SelectionTranslationHotkey>() : nullptr;
    if (!hotkey || !hotkey->owner) return false;
    const auto text = selected_ocr_text();
    if (text.isEmpty()) return false;
    try {
        hotkey->owner->translate_selection(text);
    } catch (const std::exception &e) {
        log_error(T("钉图 OCR 选区翻译失败: {e}", {{"e", QString::fromUtf8(e.what())}}), "PinShortcut");
        return false;
    }
    log_info(T("钉图 OCR 选区已直接送入翻译: {count} 字符", {{"count", static_cast<int>(text.size())}}), "PinShortcut");
    return true;
}

bool PinEditShortcutHandler::handle_key(QInputEvent *event) {
    if (typing_in_dialog(event) || menu_open(event)) return false;
    auto *pin = find_target_pin(event);
    if (!pin || !(pin->canvas && pin->canvas->is_editing())) return false;
    const int key = event_key(event);
    // Copy, copy text, toolbar (closing it also leaves editing), reset size.
    if (handle_shared(pin, event)) return true;
    if (key == Qt::Key_Escape) {
        if (clear_text_selection(pin)) return true;
        pin->stop_drawing();
        return true;
    }
    if (match(event, "inapp_delete")) {
        if (pin->view && pin->view->smart_edit_controller) pin->view->smart_edit_controller->delete_selected();
        return true;
    }
    // Other keys are not swallowed while editing.
    return false;
}

bool PinNormalShortcutHandler::handle_key(QInputEvent *event) {
    if (typing_in_dialog(event) || menu_open(event)) return false;
    auto *pin = find_target_pin(event);
    if (!pin || (pin->canvas && pin->canvas->is_editing())) return false;
    const int key = event_key(event);
    // Esc while typing in a pin's text stops that pin's editing, not the pin under the pointer.
    if (key == Qt::Key_Escape) {
        if (auto *editor = qobject_cast<PinTextEdit *>(QApplication::focusWidget())) {
            editor->pin()->text_mode_controller()->stop_editing();
            return true;
        }
    }
    if (handle_shared(pin, event)) return true;
    if (pin->handle_mouse_alternative_key(event)) return true;
    // While editing L may be typed text, so only here.
    if (match(event, "inapp_pin_lock")) {
        pin->toggle_lock();
        return true;
    }
    if (match(event, "inapp_thumbnail")) {
        if (pin->lock_blocks("move")) pin->show_locked_hint();
        else pin->toggle_thumbnail_mode();
        return true;
    }
    // A thumbnail shows no text, so the key goes on.
    if (match(event, "inapp_pin_text_mode") && !pin->thumbnail_mode()) {
        pin->toggle_text_mode();
        return true;
    }
    if (match(event, "inapp_pin_text_selection") && !pin->thumbnail_mode() && !pin->text_mode()) {
        pin->toggle_text_selection();
        return true;
    }
    // Only read-only text gets here; once editing, the editor types the key.
    if (match(event, "inapp_pin_text_edit") && pin->text_mode()) {
        pin->edit_text();
        return true;
    }
    if (key == Qt::Key_Escape) {
        if (pin->text_mode()) pin->leave_text_mode();
        else if (!clear_text_selection(pin)) pin->close_unless_locked();
        return true;
    }
    return false;
}

PinShortcutController::PinShortcutController() {
    edit_handler_ = std::make_unique<PinEditShortcutHandler>(this);
    normal_handler_ = std::make_unique<PinNormalShortcutHandler>(this);
    auto *mgr = ShortcutManager::instance();
    mgr->register_handler(edit_handler_.get());
    mgr->register_handler(normal_handler_.get());
}

namespace {
PinShortcutController *created_controller = nullptr;
}

PinShortcutController *PinShortcutController::instance() {
    static PinShortcutController *controller = created_controller = new PinShortcutController;
    return controller;
}

PinShortcutController *PinShortcutController::existing() { return created_controller; }

void PinShortcutController::reload_bindings() {
    edit_handler_->reload_bindings();
    normal_handler_->reload_bindings();
}

void PinShortcutController::register_pin(PinWindow *pin_window) {
    if (!pin_windows_.contains(pin_window)) pin_windows_.append(pin_window);
}

void PinShortcutController::unregister_pin(PinWindow *pin_window) { pin_windows_.removeAll(pin_window); }

bool PinShortcutController::matches(QInputEvent *event, const QString &cfg_key) const { return normal_handler_->match(event, cfg_key); }

PinWindow *PinShortcutController::find_pin_under_cursor() {
    // During a capture the pins are covered by it.
    if (PinManager::instance()->capture_open()) return nullptr;
    const auto cursor_pos = QCursor::pos();
    // Drops closed windows.
    QList<QPointer<PinWindow>> alive;
    for (const auto &pin : pin_windows_)
        if (pin && !pin->is_closed()) alive.append(pin);
    pin_windows_ = alive;
    // Newest first (on top). Click-through pins take no shortcuts: the user is working
    // in the window below, and Esc or Ctrl+C must not act on the pin.
    for (auto it = pin_windows_.rbegin(); it != pin_windows_.rend(); ++it) {
        auto *pin = it->data();
        if (pin->geometry().contains(cursor_pos) && !pin->click_through()) return pin;
    }
    return nullptr;
}

PinWindow *PinShortcutController::focused_pin() {
    if (PinManager::instance()->capture_open()) return nullptr;
    for (const auto &pin : pin_windows_)
        if (pin && pin->has_keyboard_focus() && !pin->click_through()) return pin;
    return nullptr;
}

PinWindow *PinShortcutController::pin_of_window(const QWidget *window) const {
    for (const auto &pin : pin_windows_) {
        if (!pin || pin->is_closed()) continue;
        for (auto *widget = window; widget; widget = widget->parentWidget())
            if (widget == pin.data() || (pin->toolbar && widget == pin->toolbar)) return pin;
    }
    return nullptr;
}

PinWindow *PinShortcutController::find_target_pin(QInputEvent *event) {
    if (auto *window = ShortcutManager::instance()->dispatch_window()) {
        // Click-through pins take no shortcuts: the user is working in the window below.
        auto *pin = pin_of_window(window);
        return pin && !pin->click_through() ? pin : nullptr;
    }
    // With the pointer moved off after selecting text, Ctrl+C must still copy that pin's text.
    if (!(event && is_mouse_shortcut_event(event)))
        if (auto *pin = focused_pin()) return pin;
    return find_pin_under_cursor();
}
}
