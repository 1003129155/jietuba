#include "pin/pin_ocr_manager.h"
#include "core/i18n.h"
#include "core/logger.h"
#include "core/platform_utils.h"
#include "pin/pin_window.h"
#include "settings/tool_settings.h"
#include <QElapsedTimer>

namespace jietuba {
void PinOcrThread::run() {
    QElapsedTimer clock;
    clock.start();
    try {
        result = recognize_text(image_);
        std::tie(prepared_items, prepared_union_rect) = OCRTextLayer::prepare_ocr_items(*result);
        log_debug(T("OCR处理完成，总耗时: {elapsed:.3f}秒", {{"elapsed", clock.nsecsElapsed() / 1e9}}), "OCR");
    } catch (const std::exception &e) {
        log_error(T("识别失败: {e}，耗时: {elapsed:.3f}秒", {{"e", QString::fromUtf8(e.what())}, {"elapsed", clock.nsecsElapsed() / 1e9}}), "OCR");
        result.reset();
        prepared_items.clear();
        prepared_union_rect.reset();
    }
    // Releases the image data.
    image_ = QImage();
}

PinOCRManager::PinOCRManager(PinWindow *pin_window, ToolSettingsManager *config_manager)
    : win_(pin_window), text_selection_enabled_(config_manager && config_manager->get_ocr_enabled()) {}

bool PinOCRManager::init_now(bool force) {
    try {
        if (ocr_thread) return true;
        // Automatic recognition runs once after pinning. Translating, copying text or turning
        // on text selection may have recognized the pin before that (the layer exists and the
        // switch is on); it is not recognized twice.
        if (!force && ocr_text_layer) return true;
        // Without a layer the switch still holds the automatic OCR setting, so one condition covers both.
        if (!force && !text_selection_enabled_) {
            log_info(T("钉图自动 OCR 已关闭，跳过自动识别"), "OCR");
            return false;
        }
        if (!is_ocr_available()) {
            log_debug(T("OCR 模块不可用（无OCR版本），静默跳过"), "OCR");
            return false;
        }
        if (!initialize_ocr()) {
            log_warning(T("OCR 引擎初始化失败"), "OCR");
            return false;
        }
        log_debug(T("OCR 引擎已就绪（支持中日韩英混合识别）"), "OCR");
        // Recognition starts now: either the switch was on already, or the user asked
        // for text and paid for recognition, so selection comes with it.
        text_selection_enabled_ = true;
        if (!ocr_text_layer) {
            ocr_text_layer = new OCRTextLayer(win_);
            const auto cr = win_->content_rect();
            ocr_text_layer->setGeometry(cr.toRect());
            // With the pin under the cursor the hover buttons show first; the layer comes 300 ms later.
            win_->raise_control_buttons();
            log_debug(T("OCR层初始化几何: {rect}", {{"rect", py_repr(cr.toRect())}}), "OCR");
        }
        // The layer may exist and have been switched off; sync the switch to it.
        apply_text_layer_enabled();
        return start_recognition();
    } catch (const std::exception &e) {
        log_exception(e, T("OCR初始化"), false);
        return false;
    }
}

bool PinOCRManager::recognize_then(Callback callback) {
    pending_callbacks_.push_back(std::move(callback));
    if (ocr_thread) return true;
    if (init_now(true)) return true;
    pending_callbacks_.pop_back();
    return false;
}

void PinOCRManager::flush_pending(bool success, const QString &result) {
    auto callbacks = std::move(pending_callbacks_);
    pending_callbacks_.clear();
    for (auto &callback : callbacks) callback(success, result);
}

bool PinOCRManager::start_recognition() {
    const auto &pixmap = win_->base_pixmap();
    if (pixmap.isNull()) return false;
    const int original_width = pixmap.width();
    const int original_height = pixmap.height();
    const auto image = win_->get_current_image();
    log_debug(T("开始异步识别文字..."), "OCR");
    ocr_thread = new PinOcrThread(image, win_);
    QObject::connect(ocr_thread, &QThread::finished, win_, [this, original_width, original_height] { on_finished(original_width, original_height); });
    ocr_thread->start();
    return true;
}

void PinOCRManager::on_finished(int original_width, int original_height) {
    try {
        if (win_->is_closed()) {
            log_debug(T("窗口已关闭，跳过结果加载"), "OCR");
        } else if (!ocr_text_layer) {
            log_debug(T("OCR 文字层已被清理，跳过结果加载"), "OCR");
        } else if (ocr_thread) {
            if (!ocr_thread->prepared_items.empty()) {
                const int text_count = static_cast<int>(ocr_thread->prepared_items.size());
                ocr_text_layer->load_prepared_ocr_items(std::move(ocr_thread->prepared_items), ocr_thread->prepared_union_rect, original_width, original_height);
                if (text_count > 0) {
                    ocr_has_result_ = true;
                    if (!pending_callbacks_.empty()) {
                        log_info(T("OCR 完成，执行等待中的操作"), "OCR");
                        flush_pending(true, ocr_text_layer->get_all_text("\n"));
                    }
                }
                log_info(T("钉图文字层已就绪，识别到 {text_count} 个文字块", {{"text_count", text_count}}), "OCR");
            } else if (!pending_callbacks_.empty()) {
                log_warning(T("OCR 未识别到文字"), "OCR");
                flush_pending(false, i18n_tr("No text was recognized", "PinWindow"));
            }
        }
    } catch (const std::exception &e) {
        log_error(T("加载OCR结果失败: {e}", {{"e", QString::fromUtf8(e.what())}}), "OCR");
        flush_pending(false, i18n_tr("OCR recognition failed: {error}", "PinWindow").replace("{error}", QString::fromUtf8(e.what())));
    }
    if (ocr_thread) {
        ocr_thread->deleteLater();
        ocr_thread = nullptr;
    }
    // Memory paged in during inference goes back once recognition ends.
    request_trim_working_set();
}

void PinOCRManager::cleanup() {
    // Callbacks belong to the closing window; nobody wants the result any more.
    pending_callbacks_.clear();
    if (ocr_thread) {
        if (ocr_thread->isRunning()) {
            log_warning(T("窗口关闭，OCR 线程仍在运行，将其安全分离..."), "OCR");
            // Detached from the window; it deletes itself when done.
            QObject::disconnect(ocr_thread, &QThread::finished, nullptr, nullptr);
            ocr_thread->setParent(nullptr);
            QObject::connect(ocr_thread, &QThread::finished, ocr_thread, &QObject::deleteLater);
        } else {
            ocr_thread->deleteLater();
        }
        ocr_thread = nullptr;
    }
    if (ocr_text_layer) {
        ocr_text_layer->set_enabled(false);
        ocr_text_layer->cleanup();
        ocr_text_layer->deleteLater();
        ocr_text_layer = nullptr;
    }
}

void PinOCRManager::set_enabled(bool enabled) {
    temporary_enabled_ = enabled;
    apply_text_layer_enabled();
}

void PinOCRManager::set_text_selection_enabled(bool enabled) {
    if (text_selection_enabled_ == enabled) return;
    text_selection_enabled_ = enabled;
    apply_text_layer_enabled();
    if (enabled && !ocr_text_layer && !ocr_thread) init_now(true);
}

bool PinOCRManager::toggle_text_selection() {
    set_text_selection_enabled(!text_selection_enabled_);
    return text_selection_enabled_;
}

void PinOCRManager::apply_text_layer_enabled() {
    if (ocr_text_layer) ocr_text_layer->set_enabled(text_selection_enabled_ && temporary_enabled_);
}

void PinOCRManager::set_drawing_mode(bool active) {
    if (ocr_text_layer) ocr_text_layer->set_drawing_mode(active);
}

void PinOCRManager::update_geometry(const QRect &rect) {
    if (ocr_text_layer) ocr_text_layer->setGeometry(rect);
}
}
