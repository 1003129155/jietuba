#include "pin/pin_shortcut.h"
#include "core/logger.h"
#include "pin/ocr_text_layer.h"
#include "pin/pin_canvas.h"
#include "pin/pin_canvas_view.h"
#include "pin/pin_manager.h"
#include "pin/pin_toolbar.h"
#include "pin/pin_window.h"
#include "canvas/smart_edit_controller.h"
#include <QApplication>
#include <QCursor>

namespace jietuba {
namespace {
const QStringList PIN_KEYS = {
    "inapp_copy_pin", "inapp_copy_pin_text", "inapp_pin_reset_size",
    "inapp_thumbnail", "inapp_toggle_toolbar", "inapp_delete", "inapp_pin_lock",
};
}

PinHandlerBase::PinHandlerBase(PinShortcutController *controller) : controller_(controller) { reload_bindings(); }

void PinHandlerBase::reload_bindings() {
    bindings_ = load_inapp_bindings(PIN_KEYS);
    mouse_bindings_ = load_inapp_mouse_bindings(PIN_KEYS);
    // Mouse reset belongs to the dedicated mouse settings (including migration).
    mouse_bindings_.remove("inapp_pin_reset_size");
}

PinWindow *PinHandlerBase::find_pin_under_cursor() const { return controller_->find_pin_under_cursor(); }

bool PinHandlerBase::typing_in_dialog(QInputEvent *event) {
    if (event->type() != QEvent::KeyPress) return false;
    auto *window = QApplication::activeWindow();
    return window && window->isModal();
}

bool PinHandlerBase::menu_open(QInputEvent *event) { return event->type() == QEvent::KeyPress && QApplication::activePopupWidget(); }

bool PinHandlerBase::match(QInputEvent *event, const QString &cfg_key) const { return match_inapp_binding(event, cfg_key, bindings_, mouse_bindings_); }

bool PinHandlerBase::handle_shared(PinWindow *pin, QInputEvent *event) {
    if (match(event, "inapp_copy_pin")) {
        auto *ocr_layer = pin->ocr_text_layer();
        if (ocr_layer && !ocr_layer->get_selected_text().isEmpty()) ocr_layer->copy_selected_text();
        else pin->copy_to_clipboard();
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
        // In thumbnail mode the size belongs to the thumbnail, as in the context menu.
        if (pin->thumbnail_mode()) return false;
        if (pin->lock_blocks("move")) pin->show_locked_hint();
        else pin->reset_to_original_size();
        return true;
    }
    return false;
}

QString PinHandlerBase::selected_ocr_text() const {
    auto *pin = find_pin_under_cursor();
    if (!pin) return {};
    auto *ocr_layer = pin->ocr_text_layer();
    if (!ocr_layer) return {};
    return ocr_layer->get_selected_text().trimmed();
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

bool PinEditShortcutHandler::is_active() {
    auto *pin = find_pin_under_cursor();
    return pin && pin->canvas && pin->canvas->is_editing();
}

bool PinEditShortcutHandler::handle_key(QInputEvent *event) {
    if (typing_in_dialog(event) || menu_open(event)) return false;
    auto *pin = find_pin_under_cursor();
    if (!pin || !(pin->canvas && pin->canvas->is_editing())) return false;
    const int key = event_key(event);
    // Copy, copy text, toolbar (closing it also leaves editing), reset size.
    if (handle_shared(pin, event)) return true;
    if (key == Qt::Key_Escape) {
        // A text selection in the OCR layer is cleared first.
        auto *ocr_layer = pin->ocr_text_layer();
        if (ocr_layer && ocr_layer->is_active() && (ocr_layer->selection_start || ocr_layer->selection_end)) {
            ocr_layer->clear_selection();
            return true;
        }
        pin->canvas->deactivate_tool();
        if (pin->toolbar && pin->toolbar->current_tool) {
            for (auto *button : pin->toolbar->tool_buttons) button->setChecked(false);
            pin->toolbar->current_tool.reset();
        }
        return true;
    }
    if (match(event, "inapp_delete")) {
        if (pin->view && pin->view->smart_edit_controller) pin->view->smart_edit_controller->delete_selected();
        return true;
    }
    // Other keys are not swallowed while editing.
    return false;
}

bool PinNormalShortcutHandler::is_active() {
    auto *pin = find_pin_under_cursor();
    return pin && !(pin->canvas && pin->canvas->is_editing());
}

bool PinNormalShortcutHandler::handle_key(QInputEvent *event) {
    if (typing_in_dialog(event) || menu_open(event)) return false;
    auto *pin = find_pin_under_cursor();
    if (!pin || (pin->canvas && pin->canvas->is_editing())) return false;
    const int key = event_key(event);
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
    if (key == Qt::Key_Escape) {
        pin->close_unless_locked();
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

PinWindow *PinShortcutController::find_pin_under_cursor() {
    // During a capture pins sit below the capture layer. Keys the capture window does
    // not take (such as copy before confirming) keep going down and must not reach an invisible pin.
    if (PinManager::instance()->topmost_suppressed()) return nullptr;
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
}
