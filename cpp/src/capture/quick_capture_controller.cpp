#include "capture/quick_capture_controller.h"
#include "canvas/scene.h"
#include "capture/capture_service.h"
#include "capture/system_cursor.h"
#include "core/clipboard_utils.h"
#include "core/last_capture_region.h"
#include "core/logger.h"
#include "core/platform_utils.h"
#include "core/qt_utils.h"
#include "core/quick_capture_input.h"
#include "main_app.h"
#include "pin/pin_manager.h"
#include "settings/tool_settings.h"
#include "text_recognition/result_window.h"
#include "tools/watermark.h"
#include "translation/translation_manager.h"
#include "ui/quick_capture_overlay.h"
#include "ui/screenshot_window.h"
#include <QApplication>
#include <QPixmap>
#include <QScreen>
#include <algorithm>
#include <stdexcept>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>

namespace jietuba {
namespace {
// Capture actions: (copy to clipboard, pin).
std::optional<std::pair<bool, bool>> capture_action(const QString &action) {
    if (action == "copy") return std::make_pair(true, false);
    if (action == "pin") return std::make_pair(false, true);
    if (action == "copy_pin") return std::make_pair(true, true);
    return std::nullopt;
}

bool is_text_action(const QString &action) { return action == "translate" || action == "ocr"; }

QStringList sorted(QStringList list) {
    std::sort(list.begin(), list.end());
    return list;
}
}

QRect desktop_bounds() {
    QRect bounds;
    for (auto *screen : QApplication::screens()) bounds = bounds.united(screen->geometry());
    return bounds;
}

void flush_desktop() { DwmFlush(); }

void QuickCaptureWorker::run() {
    try {
        flush_desktop();
        if (isInterruptionRequested()) return;
        if (include_cursor) cursor = SystemCursor::grab();
        CaptureService service;
        QImage image;
        QRectF bounds;
        if (action == "edit") {
            // A normal capture needs the whole desktop as its base, so the selection can still change.
            std::tie(image, bounds) = service.capture_all_screens(cursor);
        } else {
            image = service.capture_region(region, cursor);
            bounds = QRectF(region);
        }
        if (image.isNull()) throw std::runtime_error("The captured image is empty");
        if (!isInterruptionRequested()) emit captured(image, bounds);
    } catch (const std::exception &e) {
        emit failed(QString::fromUtf8(e.what()));
    }
}

QuickCaptureController::QuickCaptureController(MainApp *app) : QObject(app), main_app(app) {
    input = new QuickCaptureInput(this);
    connect(input, &QuickCaptureInput::event, this, &QuickCaptureController::on_input, Qt::QueuedConnection);
    connect(input, &QuickCaptureInput::moved, this, &QuickCaptureController::on_moved, Qt::QueuedConnection);
    connect(input, &QuickCaptureInput::failure, this, &QuickCaptureController::on_failure, Qt::QueuedConnection);
}

void QuickCaptureController::refresh() {
    cancel();
    auto *config = main_app->config_manager;
    actions_ = get_quick_capture_bindings(config);
    enabled_ = !actions_.empty() && !closed_ && !config->get_app_setting("global_hotkeys_disabled", false).toBool();
    sync_input_availability();
    std::vector<std::pair<QStringList, QString>> bindings;
    QStringList gestures;
    for (const auto &[binding, action] : actions_) {
        bindings.push_back(binding);
        gestures << QString("%1+%2=%3").arg(sorted(binding.first).join('+'), binding.second, action);
    }
    input->configure(bindings, enabled_);
    log_debug(QString("Quick capture bindings applied: [%1], enabled=%2").arg(gestures.join(", "), enabled_ ? "True" : "False"), "QuickCapture");
}

void QuickCaptureController::suspend() {
    cancel();
    enabled_ = false;
    input->configure({}, false);
}

bool QuickCaptureController::blocked() const {
    auto *screenshot = main_app->screenshot_window.data();
    return main_app->update_preparing || blocking_modal() != nullptr || (screenshot && screenshot->session_active());
}

void QuickCaptureController::set_capture_pending(bool pending) {
    capture_pending_ = pending;
    sync_input_availability();
}

void QuickCaptureController::sync_input_availability() {
    if (closed_) return;
    input->set_blocked(capture_pending_ || blocked());
}

void QuickCaptureController::on_input(const QString &kind, int token, int x, int y) {
    if (kind != "cancel" && !input->accepts(token)) return;
    if (kind == "start") {
        std::optional<QString> action;
        if (const auto binding = input->gesture_binding(token)) {
            for (const auto &[candidate, name] : actions_)
                if (sorted(candidate.first) == binding->first && candidate.second == binding->second) {
                    action = name;
                    break;
                }
        }
        if (!enabled_ || !action || busy() || blocked()) {
            log_debug("Quick capture ignored: disabled, busy, or a capture/modal window is active", "QuickCapture");
            input->cancel();
            return;
        }
        bounds_ = desktop_bounds();
        start_ = QPoint(x, y);
        if (!bounds_.contains(start_)) {
            input->cancel();
            return;
        }
        active_ = token;
        active_action_ = *action;
        if (!overlay) overlay = new QuickCaptureOverlay(main_app->config_manager);
        overlay->show_selection(start_, start_, bounds_);
    } else if (active_ && token == *active_) {
        if (kind == "cancel") {
            cancel();
        } else if (kind == "finish") {
            const auto region = selection_rect(start_, QPoint(x, y), desktop_bounds());
            hide_selection();
            if (region.width() >= 2 && region.height() >= 2 && !blocked()) capture(region);
            else schedule_working_set_trim();
        }
    }
}

void QuickCaptureController::on_moved(int token) {
    const auto position = input->take_position(token);
    if (!position || !active_ || token != *active_) return;
    if (blocked()) {
        cancel();
        return;
    }
    overlay->show_selection(start_, *position, bounds_);
}

bool QuickCaptureController::hide_selection() {
    const bool was_active = active_.has_value();
    active_.reset();
    if (overlay) overlay->dismiss();
    return was_active;
}

void QuickCaptureController::schedule_working_set_trim() {
    if (!closed_ && !busy()) request_trim_working_set(1500);
}

void QuickCaptureController::cancel() {
    input->cancel();
    if (hide_selection()) schedule_working_set_trim();
    result_valid_ = false;
    if (worker_) worker_->requestInterruption();
}

void QuickCaptureController::capture(const QRect &region) {
    result_valid_ = true;
    const auto action = active_action_;
    auto *worker = new QuickCaptureWorker(region, action, this);
    // Recognition and translation want the text; a drawn pointer would only cover it.
    worker->include_cursor = !is_text_action(action) && main_app->config_manager->get_app_setting("capture_include_cursor", false).toBool();
    worker_ = worker;
    connect(worker, &QuickCaptureWorker::captured, this, &QuickCaptureController::deliver, Qt::QueuedConnection);
    connect(worker, &QuickCaptureWorker::failed, this, &QuickCaptureController::on_failure, Qt::QueuedConnection);
    connect(worker, &QThread::finished, this, [this, worker] { worker_finished(worker); });
    worker->start();
}

void QuickCaptureController::deliver(const QImage &captured_image, const QRectF &bounds) {
    auto *worker = worker_;
    if (!result_valid_ || closed_ || !worker || blocked()) return;
    auto region = worker->region;
    const auto action = worker->action;
    auto *config = main_app->config_manager;
    try {
        if (action == "edit") {
            region = region.intersected(bounds.toAlignedRect());
            if (region.isEmpty()) return;
            open_normal_capture(captured_image, bounds, worker->cursor, region);
        } else if (action == "translate") {
            TranslationManager::instance()->translate_from_image(QPixmap::fromImage(captured_image), config->get_translation_request_params());
        } else if (action == "ocr") {
            // As the toolbar's text recognition: a result window or a direct copy per settings.
            if (config->get_ocr_copy_directly_enabled()) copy_text_recognition(captured_image);
            else show_text_recognition(captured_image, region);
        } else {
            const auto [copy, pin] = capture_action(action).value_or(std::make_pair(false, false));
            // No canvas involved: with "add the last watermark" on it is drawn straight onto the image.
            const auto image = WatermarkTool::apply_to_image(captured_image, config);
            const bool watermarked = image.cacheKey() != captured_image.cacheKey();
            deliver_screenshot(image, config, copy, pin ? "pin" : "capture", {}, watermarked ? captured_image : QImage());
            if (pin) PinManager::instance()->create_pin(image, region.topLeft(), config);
        }
        set_last_region(region);
    } catch (const std::exception &e) {
        on_failure(QString::fromUtf8(e.what()));
    }
}

void QuickCaptureController::open_normal_capture(const QImage &image, const QRectF &bounds, std::shared_ptr<SystemCursor> cursor, const QRect &region) {
    main_app->on_capture_ready(image, bounds, std::move(cursor));
    auto *window = main_app->screenshot_window.data();
    if (window && window->session_active()) window->scene->preset_selection(QRectF(region));
}

void QuickCaptureController::worker_finished(QuickCaptureWorker *worker) {
    if (worker == worker_) worker_ = nullptr;
    worker->deleteLater();
    schedule_working_set_trim();
}

void QuickCaptureController::on_failure(const QString &message) {
    log_error("Quick capture failed: " + message, "QuickCapture");
    cancel();
    if (closed_) return;
    if (auto *tray = main_app->tray_icon)
        tray->showMessage(QApplication::translate("SettingsDialog", "Global Mouse Shortcuts"), QApplication::translate("SettingsDialog", "Capture failed. Please try again."),
                          QSystemTrayIcon::Warning);
}

void QuickCaptureController::close() {
    if (closed_) return;
    closed_ = true;
    cancel();
    input->close();
    if (worker_) worker_->wait();
    if (overlay) {
        overlay->close();
        overlay->deleteLater();
        overlay = nullptr;
    }
}
}
