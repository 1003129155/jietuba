#include "tools/action.h"
#include "barcode/barcode.h"
#include "canvas/scene.h"
#include "canvas/selection_model.h"
#include "core/background_tasks.h"
#include "core/clipboard_utils.h"
#include "core/i18n.h"
#include "core/last_capture_region.h"
#include "core/logger.h"
#include "core/save.h"
#include "core/ui_scale.h"
#include "pin/pin_manager.h"
#include "pin/pin_window.h"
#include "settings/tool_settings.h"
#include "text_recognition/text_recognition.h"
#include "tools/controller.h"
#include "tools/number.h"
#include "translation/translation_manager.h"
#include "ui/dialogs.h"
#include <QDateTime>
#include <QFileInfo>
#include <QGraphicsItem>
#include <QPixmap>
#include <QRegularExpression>

namespace jietuba {
namespace {
QString action_tr(const char *text) { return i18n_tr(text, "ActionTools"); }
}

ActionTools::ActionTools(CanvasScene *scene, ToolSettingsManager *config_manager, QWidget *parent_window)
    : scene(scene), config_manager(config_manager), parent_window(parent_window), export_service(scene),
      save_service(std::make_shared<SaveService>(config_manager ? *config_manager : *get_tool_settings_manager())) {}

void ActionTools::copy_save_and_close(bool close_after) {
    temporarily_exit_editing();
    const auto selection_rect = scene->selection_model->rect();
    remember_last_region(selection_rect);
    const auto image = export_service.export_image(selection_rect);
    // The image is in hand, so the window can go before the rest.
    if (parent_window && close_after) parent_window->hide();
    QImage original;
    if (has_annotations(scene->get_drawing_items_in_rect(selection_rect))) original = export_service.export_base_image_only(selection_rect);
    deliver_screenshot(image, config_manager, true, "capture", {}, original, save_service);
    log_debug(T("已交给后台复制到剪贴板"), "Action");
    if (parent_window && close_after) cleanup_and_close();
}

void ActionTools::handle_confirm() { copy_save_and_close(); }

void ActionTools::handle_copy() { copy_save_and_close(); }

void ActionTools::handle_save(bool close_after) {
    temporarily_exit_editing();
    const auto fmt = (config_manager ? config_manager->get_screenshot_format() : QString("PNG")).toLower();
    const auto default_name = QString("screenshot_%1.%2").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"), fmt);
    auto [file_path, selected_filter] = get_save_file_name(parent_window, action_tr("Save Screenshot"), save_service->suggest_path(default_name),
                                                           "PNG (*.png);;JPG (*.jpg);;BMP (*.bmp);;WebP (*.webp);;PDF (*.pdf)");
    if (file_path.isEmpty()) return;
    const auto image_format = format_from_save_path(file_path, selected_filter);
    if (QFileInfo(file_path).suffix().isEmpty()) file_path = file_path + "." + image_format.toLower();
    auto selection_rect = scene->selection_model->rect();
    remember_last_region(selection_rect);
    if (selection_rect.isEmpty()) selection_rect = scene->sceneRect();
    const auto image = export_service.export_image(selection_rect);
    if (SaveService::save_qimage_to_path(image, file_path, image_format)) {
        log_info(T("已保存到: {file_path}", {{"file_path", file_path}}), "Action");
        if (parent_window && close_after) cleanup_and_close();
    } else {
        show_warning_dialog(parent_window, action_tr("Save Failed"), file_path);
    }
}

void ActionTools::handle_pin(bool close_after) {
    temporarily_exit_editing();
    const auto selection_rect = scene->selection_model->rect();
    remember_last_region(selection_rect);
    // The base image only, without annotations.
    const auto base_image = export_service.export_base_image_only(selection_rect);
    const auto drawing_items = scene->get_drawing_items_in_rect(selection_rect);
    const QPoint position(static_cast<int>(python_round(selection_rect.x())), static_cast<int>(python_round(selection_rect.y())));
    auto *pin_window = PinManager::instance()->create_pin(base_image, position, config_manager, drawing_items,
                                                          QPoint(static_cast<int>(selection_rect.x()), static_cast<int>(selection_rect.y())),
                                                          NumberTool::get_next_number(scene));
    pin_window->show();
    log_debug(T("已创建钉图窗口"), "PinAction");
    log_debug(T("位置: ({pos_x}, {pos_y})", {{"pos_x", position.x()}, {"pos_y", position.y()}}), "PinAction");
    log_debug(T("底图: {width}x{height}", {{"width", base_image.width()}, {"height", base_image.height()}}), "PinAction");
    log_debug(T("继承绘制项目: {count} 个", {{"count", static_cast<int>(drawing_items.size())}}), "PinAction");
    const auto result_image = export_service.export_image(selection_rect);
    // The image is in hand, so the capture window can go early.
    if (parent_window && close_after) parent_window->hide();
    const auto original = has_annotations(drawing_items) ? base_image : QImage();
    deliver_screenshot(result_image, config_manager, true, "pin", {}, original, save_service);
    log_debug(T("已交给后台复制到剪贴板"), "PinAction");
    if (parent_window && close_after) cleanup_and_close();
}

bool ActionTools::handle_capture_action(const QString &action) {
    // Close works without a selection, like Esc, so it also exits before selecting.
    if (action == "close") {
        cleanup_and_close();
        return true;
    }
    if (!scene->selection_model->is_confirmed() || scene->selection_model->rect().isEmpty()) return false;
    if (action == "copy") {
        handle_copy();
    } else if (action == "pin") {
        handle_pin();
    } else if (action == "save") {
        handle_save();
    } else if (action == "quick_save") {
        temporarily_exit_editing();
        const auto rect = scene->selection_model->rect();
        remember_last_region(rect);
        const auto image = export_service.export_image(rect);
        const auto [success, path] = save_service->save_qimage(image, config_manager->get_screenshot_save_path(), "", config_manager->get_screenshot_format());
        if (success) {
            save_original_beside(rect, path.value_or(QString()));
            cleanup_and_close();
        } else {
            show_warning_dialog(parent_window, action_tr("Save Failed"), path.value_or(QString()));
        }
    } else {
        return false;
    }
    return true;
}

bool ActionTools::has_annotations(const QList<QGraphicsItem *> &drawing_items) {
    return std::any_of(drawing_items.begin(), drawing_items.end(), [](QGraphicsItem *item) { return item->isVisible(); });
}

void ActionTools::save_original_beside(const QRectF &selection_rect, const QString &saved_path) {
    if (!config_manager->get_screenshot_save_original_enabled()) return;
    if (!has_annotations(scene->get_drawing_items_in_rect(selection_rect))) return;
    const auto original = export_service.export_base_image_only(selection_rect);
    background_tasks::start_thread([original, saved_path] { SaveService::save_beside(original, saved_path, "original"); }, "SaveOriginal");
}

void ActionTools::handle_screenshot_translate() {
    log_info(T("启动截图翻译模式"), "ScreenshotTranslate");
    const auto base_image = selection_base_image();
    if (base_image.isNull()) return;
    // The OCR thread works on a pixmap copy.
    const auto pixmap_copy = QPixmap::fromImage(base_image);
    log_debug(T("已复制底图用于OCR: {width}x{height}", {{"width", pixmap_copy.width()}, {"height", pixmap_copy.height()}}), "ScreenshotTranslate");
    // Read before the window closes.
    const auto params = config_manager ? config_manager->get_translation_request_params() : QVariantMap();
    log_debug(T("关闭截图窗口，释放内存"), "ScreenshotTranslate");
    cleanup_and_close();
    TranslationManager::instance()->translate_from_image(pixmap_copy, params);
}

void ActionTools::handle_text_recognize() {
    const auto image = selection_base_image();
    if (image.isNull()) return;
    // The result window is placed by the selection, read before the capture closes.
    const auto anchor = scene->selection_model->rect().toAlignedRect();
    cleanup_and_close();
    if (config_manager && config_manager->get_ocr_copy_directly_enabled()) copy_text_recognition(image);
    else show_text_recognition(image, anchor);
}

void ActionTools::handle_scan_code() {
    const auto image = selection_base_image();
    if (image.isNull()) return;
    cleanup_and_close();
    const bool copy_single = config_manager && config_manager->get_barcode_copy_single_enabled();
    show_barcode_result(image, copy_single);
}

QImage ActionTools::selection_base_image() {
    if (!scene->selection_model->is_confirmed()) {
        show_modeless_warning_dialog(parent_window, action_tr("Warning"), action_tr("Please select a valid capture area first."));
        return {};
    }
    const auto selection_rect = scene->selection_model->rect();
    remember_last_region(selection_rect);
    const auto image = export_service.export_base_image_only(selection_rect);
    if (image.isNull()) {
        show_modeless_warning_dialog(parent_window, action_tr("Error"), action_tr("Failed to capture the selected area."));
        return {};
    }
    return image;
}

void ActionTools::remember_last_region(const QRectF &rect) {
    if (rect.isEmpty()) return;
    set_last_region(QRect(static_cast<int>(python_round(rect.x())), static_cast<int>(python_round(rect.y())), static_cast<int>(python_round(rect.width())),
                          static_cast<int>(python_round(rect.height()))));
}

void ActionTools::temporarily_exit_editing() {
    if (!scene || !scene->tool_controller) return;
    auto *tool_controller = scene->tool_controller.get();
    const auto current_tool_id = tool_controller->current_tool_id();
    if (current_tool_id != "cursor") {
        log_debug(T("从 {tool_id} 切换到 cursor", {{"tool_id", current_tool_id}}), "Action");
        tool_controller->activate("cursor");
    }
    // Clears smart-edit selection and the brush indicator through a signal, so actions never reach into the view.
    emit scene->editing_cleanup_requested();
}

void ActionTools::cleanup_and_close() {
    if (!parent_window) return;
    if (auto *host = dynamic_cast<ActionHost *>(parent_window.data())) host->cleanup_and_close();
    else parent_window->close();
}

QString ActionTools::format_from_save_path(const QString &file_path, const QString &selected_filter) {
    const auto ext = QFileInfo(file_path).suffix();
    if (!ext.isEmpty()) return ext.toUpper();
    // The first extension of the filter, e.g. "JPEG (*.jpg *.jpeg)" -> "jpg".
    static const QRegularExpression pattern(R"(\*\.(\w+))");
    const auto match = pattern.match(selected_filter);
    if (match.hasMatch()) return match.captured(1).toUpper();
    return "PNG";
}
}
