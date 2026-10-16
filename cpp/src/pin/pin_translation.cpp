#include "pin/pin_translation.h"
#include "core/i18n.h"
#include "core/logger.h"
#include "pin/ocr_text_layer.h"
#include "pin/pin_window.h"
#include "settings/tool_settings.h"
#include "translation/translation_dialog.h"
#include "translation/translation_manager.h"
#include "ui/dialogs.h"
#include <QApplication>
#include <QScreen>

namespace jietuba {
namespace {
QVariantMap request_params(ToolSettingsManager *config_manager) {
    return config_manager ? config_manager->get_translation_request_params() : QVariantMap{{"target_lang", "ZH"}};
}
}

bool PinTranslationHelper::translate(OCRTextLayer *ocr_text_layer) {
    try {
        if (!ocr_text_layer || !ocr_text_layer->has_text()) {
            log_warning(T("没有可翻译的文字"), "Translate");
            return false;
        }
        const auto all_text = ocr_text_layer->get_all_text("\n");
        if (all_text.trimmed().isEmpty()) {
            log_warning(T("OCR 文字为空"), "Translate");
            return false;
        }
        log_info(T("准备翻译 {count} 个字符", {{"count", static_cast<int>(all_text.toUcs4().size())}}), "Translate");
        TranslationManager::instance()->translate(all_text, calculate_window_position(), request_params(config_manager));
        return true;
    } catch (const std::exception &e) {
        log_error(T("翻译启动失败: {e}", {{"e", QString::fromUtf8(e.what())}}), "Translate");
        return false;
    }
}

bool PinTranslationHelper::begin_ocr_translation() {
    try {
        TranslationManager::instance()->begin_ocr_translation(calculate_window_position(), request_params(config_manager));
        return true;
    } catch (const std::exception &e) {
        log_error(T("翻译启动失败: {e}", {{"e", QString::fromUtf8(e.what())}}), "Translate");
        return false;
    }
}

void PinTranslationHelper::complete_ocr_translation(bool success, const QString &result) { TranslationManager::instance()->complete_ocr_translation(success, result); }

QPoint PinTranslationHelper::calculate_window_position() const {
    const int gap = 10;
    const auto pin_global_pos = parent->mapToGlobal(QPoint(0, 0));
    const int pin_x = pin_global_pos.x(), pin_y = pin_global_pos.y();
    const int pin_width = parent->width();
    auto *screen = QApplication::screenAt(pin_global_pos);
    if (!screen) screen = QApplication::primaryScreen();
    const auto screen_geometry = screen->availableGeometry();
    const auto [window_width, window_height] = TranslationDialog::initial_size_for_available_geometry(screen_geometry);
    const int screen_left = screen_geometry.left();
    const int screen_right = screen_left + screen_geometry.width();
    const int screen_top = screen_geometry.top();
    const int screen_bottom = screen_top + screen_geometry.height();
    int final_x;
    const int right_x = pin_x + pin_width + gap;
    if (right_x + window_width <= screen_right) {
        final_x = right_x;
    } else {
        const int left_x = pin_x - window_width - gap;
        if (left_x >= screen_left) {
            final_x = left_x;
        } else {
            // Neither side fits: kept within this screen's available area.
            const int max_x = std::max(screen_left, screen_right - window_width);
            final_x = std::min(std::max(pin_x, screen_left), max_x);
        }
    }
    const int max_y = std::max(screen_top, screen_bottom - window_height);
    const int final_y = std::min(std::max(pin_y, screen_top), max_y);
    return QPoint(final_x, final_y);
}

void PinTranslationHelper::show_error(const QString &message) { show_warning_dialog(parent, i18n_tr("Translation Error", "PinTranslationHelper"), message); }
}
