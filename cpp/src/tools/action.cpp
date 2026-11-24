#include "tools/action.h"
#include "barcode/barcode.h"
#include "canvas/items/background_item.h"
#include "canvas/scene.h"
#include "canvas/selection_model.h"
#include "canvas/smart_erase_wait.h"
#include "core/background_tasks.h"
#include "core/clipboard_utils.h"
#include "core/i18n.h"
#include "core/last_capture_region.h"
#include "core/logger.h"
#include "core/save.h"
#include "core/file_names.h"
#include "core/ui_scale.h"
#include "pin/pin_manager.h"
#include "pin/pin_window.h"
#include "settings/tool_settings.h"
#include "text_recognition/text_recognition.h"
#include "tools/controller.h"
#include "tools/number.h"
#include "translation/overlay_translation.h"
#include "translation/overlay_translation_session.h"
#include "translation/service.h"
#include "translation/translation_manager.h"
#include "upload/upload_service.h"
#include "ui/dialogs.h"
#include "ui/save_image_dialog.h"
#include "ui/toast.h"
#include "ui/overlay_translation_indicator.h"
#include "ui/wait_bar.h"
#include <QApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QGraphicsItem>
#include <QPixmap>
#include <QRegularExpression>
#include <QThread>
#include <QTimer>

namespace jietuba {
namespace {
QString action_tr(const char *text) { return i18n_tr(text, "ActionTools"); }
// A capture handed over shows that it is still at work only when that takes a while.
constexpr int WAIT_BAR_DELAY_MS = 200;
// A selection changed without a drag is taken as resting after this long.
constexpr int SELECTION_REST_MS = 300;
}

ActionTools::ActionTools(CanvasScene *scene, ToolSettingsManager *config_manager, QWidget *parent_window)
    : scene(scene), config_manager(config_manager), parent_window(parent_window), export_service(scene),
      save_service(std::make_shared<SaveService>(config_manager ? *config_manager : *get_tool_settings_manager())) {}

void ActionTools::finish_when_ready(const QRectF &selection_rect, std::function<void()> finish) {
    auto *host = dynamic_cast<ActionHost *>(parent_window.data());
    if (!host || !smart_erase_pending(scene, selection_rect)) {
        finish();
        return;
    }
    // Scene coordinates are screen coordinates.
    auto *bar = new WaitBar(nullptr, QSize(scaled(260), scaled(14)));
    bar->set_progress(0, 0);
    bar->center_on(selection_rect.center().toPoint());
    bar->show_after(WAIT_BAR_DELAY_MS);
    QObject *context = host->hand_over();
    QObject::connect(context, &QObject::destroyed, bar, &QObject::deleteLater);
    when_smart_erase_ready(scene, selection_rect, context, [bar, finish = std::move(finish)] {
        bar->dismiss();
        bar->deleteLater();
        finish();
    });
}

void ActionTools::copy_save_and_close(bool close_after, bool as_link) {
    temporarily_exit_editing();
    const auto selection_rect = scene->selection_model->rect();
    remember_last_region(selection_rect);
    const auto finish = [this, selection_rect, close_after, as_link] {
        const auto image = export_service.export_image(selection_rect);
        // The image is in hand, so the window can go before the rest.
        if (parent_window && close_after) parent_window->hide();
        QImage original;
        if (has_annotations(scene->get_drawing_items_in_rect(selection_rect))) original = export_service.export_base_image_only(selection_rect);
        // An upload leaves the clipboard to the link.
        deliver_screenshot(image, config_manager, !as_link, "capture", {}, original, save_service);
        if (as_link) upload::start_upload(image, save_service->config_manager, upload::Source::Screenshot);
        else log_debug(T("已交给后台复制到剪贴板"), "Action");
        if (parent_window && close_after) cleanup_and_close();
    };
    if (close_after) finish_when_ready(selection_rect, finish);
    else finish();
}

void ActionTools::handle_confirm() { copy_save_and_close(); }

void ActionTools::handle_copy() { copy_save_and_close(); }

void ActionTools::handle_save(bool close_after) {
    temporarily_exit_editing();
    auto selection_rect = scene->selection_model->rect();
    remember_last_region(selection_rect);
    if (selection_rect.isEmpty()) selection_rect = scene->sceneRect();
    const auto base_name = file_names::stem(save_service->config_manager, file_names::Screenshot);
    if (!parent_window || !close_after) {
        save_image_interactively(parent_window, export_service.export_image(selection_rect), base_name, config_manager, save_service);
        return;
    }
    // A smart erase still rebuilding: the capture goes at once and the save window opens when it is done.
    finish_when_ready(selection_rect, [this, selection_rect, base_name] {
        const auto image = export_service.export_image(selection_rect);
        // The capture ends before the save window opens. That window runs once the click
        // asking for it has returned, owning nothing of the capture, which is being deleted.
        cleanup_and_close();
        QTimer::singleShot(0, qApp, [image, base_name, config = config_manager, service = save_service] {
            save_image_interactively(nullptr, image, base_name, config, service);
        });
    });
}

void ActionTools::handle_quick_save() {
    temporarily_exit_editing();
    const auto rect = scene->selection_model->rect();
    remember_last_region(rect);
    finish_when_ready(rect, [this, rect] {
        const auto image = export_service.export_image(rect);
        const auto [success, path] = save_service->save_qimage(image, config_manager->get_screenshot_save_path(), "", "", config_manager->get_screenshot_format(),
                                                               save_service->options());
        // A path comes back even when the write failed, so it doubles as the detail line;
        // no path at all means the folder or the file name was the problem.
        const auto failure_detail = path ? *path : action_tr("Cannot create the save folder or reserve a file name.");
        if (success) {
            save_original_beside(rect, path.value_or(QString()));
            cleanup_and_close();
            // Nothing else shows that the file was written.
            show_toast(action_tr("Saved: %1").replace("%1", QFileInfo(path.value_or(QString())).fileName()));
        } else if (parent_window && parent_window->isVisible()) {
            show_warning_dialog(parent_window, action_tr("Save Failed"), failure_detail);
        } else {
            // Handed over: the capture cannot come back, so it ends before the warning.
            cleanup_and_close();
            show_warning_dialog(nullptr, action_tr("Save Failed"), failure_detail);
        }
    });
}

void ActionTools::handle_upload() {
    if (!upload::default_target(save_service->config_manager)) {
        upload::show_no_target_hint();
        return;
    }
    copy_save_and_close(true, true);
}

void ActionTools::save_image_interactively(QWidget *parent, const QImage &image, const QString &base_name, ToolSettingsManager *config,
                                           const std::shared_ptr<SaveService> &service) {
    if (use_modern_save_dialog(config)) {
        const auto [outcome, path] = SaveImageDialog::run(parent, image, base_name, config);
        if (outcome == SaveImageDialog::Outcome::SAVED) log_info(T("已保存到: {file_path}", {{"file_path", path}}), "Action");
        if (outcome != SaveImageDialog::Outcome::CLASSIC) return;
    }
    const auto fmt = (config ? config->get_screenshot_format() : QString("PNG")).toLower();
    auto [file_path, selected_filter] = get_save_file_name(parent, action_tr("Save Screenshot"), service->suggest_path(base_name + "." + fmt),
                                                           "PNG (*.png);;JPG (*.jpg);;BMP (*.bmp);;WebP (*.webp);;PDF (*.pdf)");
    if (file_path.isEmpty()) return;
    const auto image_format = format_from_save_path(file_path, selected_filter);
    if (QFileInfo(file_path).suffix().isEmpty()) file_path = file_path + "." + image_format.toLower();
    if (SaveService::save_qimage_to_path(image, file_path, image_format, service->options())) log_info(T("已保存到: {file_path}", {{"file_path", file_path}}), "Action");
    else show_warning_dialog(parent, action_tr("Save Failed"), file_path);
}

void ActionTools::handle_pin(bool close_after) {
    temporarily_exit_editing();
    const auto selection_rect = scene->selection_model->rect();
    // Declined, the capture stays open as it was.
    if (!PinManager::confirm_small_pin(selection_rect.size().toSize(), parent_window)) return;
    remember_last_region(selection_rect);
    // A translation laid over the selection goes on with the pin as a layer of its own, so the pin
    // keeps the picture under it and can show either.
    auto *translation = overlay_session_.data();
    const bool carried = translation && translation->shown() && !translation->blocks().isEmpty() &&
                         translation->region() == translation->region_of(selection_rect);
    const bool layered = carried && scene->background->overlay_visible();
    if (layered) scene->background->set_overlay_visible(false);
    // The base image only, without annotations.
    const auto base_image = export_service.export_base_image_only(selection_rect);
    if (layered) scene->background->set_overlay_visible(true);
    const auto drawing_items = scene->get_drawing_items_in_rect(selection_rect);
    const QPoint position(static_cast<int>(python_round(selection_rect.x())), static_cast<int>(python_round(selection_rect.y())));
    auto *pin_window = PinManager::instance()->create_pin(base_image, position, config_manager, drawing_items,
                                                          QPoint(static_cast<int>(selection_rect.x()), static_cast<int>(selection_rect.y())),
                                                          NumberTool::get_next_number(scene));
    pin_window->show();
    if (carried) pin_window->adopt_translation(translation->blocks(), translation->original_shown(), translation->busy());
    log_debug(T("已创建钉图窗口"), "PinAction");
    log_debug(T("位置: ({pos_x}, {pos_y})", {{"pos_x", position.x()}, {"pos_y", position.y()}}), "PinAction");
    log_debug(T("底图: {width}x{height}", {{"width", base_image.width()}, {"height", base_image.height()}}), "PinAction");
    log_debug(T("继承绘制项目: {count} 个", {{"count", static_cast<int>(drawing_items.size())}}), "PinAction");
    // The pin takes the focus before the capture window goes, so it never falls back to the previous window in between.
    if (parent_window && close_after) pin_window->activateWindow();
    const auto original = has_annotations(drawing_items) ? base_image : QImage();
    // The pin finishes a smart erase still rebuilding by itself; only the copy for the clipboard waits.
    const auto finish = [this, selection_rect, original, close_after] {
        const auto result_image = export_service.export_image(selection_rect);
        if (parent_window && close_after) parent_window->hide();
        deliver_screenshot(result_image, config_manager, true, "pin", {}, original, save_service);
        log_debug(T("已交给后台复制到剪贴板"), "PinAction");
        if (parent_window && close_after) cleanup_and_close();
    };
    if (close_after) finish_when_ready(selection_rect, finish);
    else finish();
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
        handle_quick_save();
    } else if (action == "upload") {
        handle_upload();
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
    const auto suffix = file_names::load(*config_manager).at(file_names::OriginalSuffix);
    background_tasks::start_thread([original, saved_path, suffix, options = save_service->options()] { SaveService::save_beside(original, saved_path, suffix, options); },
                                   "SaveOriginal");
}

void ActionTools::handle_screenshot_translate() {
    if (!config_manager || config_manager->get_screenshot_translate_mode() == "compare") handle_screenshot_translate_compare();
    else handle_screenshot_translate_overlay();
}

void ActionTools::handle_screenshot_translate_compare() {
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

void ActionTools::handle_screenshot_translate_overlay() { show_overlay_translation(); }

bool ActionTools::show_overlay_translation() {
    if (!scene || !scene->selection_model->is_confirmed() || scene->selection_model->rect().isEmpty()) {
        show_modeless_warning_dialog(parent_window, action_tr("Warning"), action_tr("Please select a valid capture area first."));
        return false;
    }
    // Selection frames and handles must not end up in the recognized pixels.
    temporarily_exit_editing();
    const auto selection = scene->selection_model->rect();
    remember_last_region(selection);
    log_info(T("启动覆盖翻译: {width}x{height}", {{"width", static_cast<int>(selection.width())}, {"height", static_cast<int>(selection.height())}}),
             "ScreenshotTranslate");
    return overlay_translation()->show(selection);
}

OverlayTranslationSession *ActionTools::overlay_translation() {
    if (overlay_session_) return overlay_session_;
    // It goes with the scene, whichever way the capture ends: no more requests are sent for a
    // capture that is gone.
    auto *session = new OverlayTranslationSession(scene, config_manager, scene);
    overlay_session_ = session;
    const QPointer<QWidget> parent = parent_window;
    // The wait is swept over the selection until the run ends.
    if (auto *view = scene->views().value(0)) {
        auto *indicator = new OverlayTranslationIndicator(view);
        QObject::connect(session, &OverlayTranslationSession::waiting_changed, indicator, [indicator, session] { indicator->set_waiting(session->waiting()); });
        QObject::connect(session, &QObject::destroyed, indicator, &QObject::deleteLater);
    }
    // The layer belongs to the pixels it was translated from: moving the selection takes it
    // off, and the translation follows once the selection rests. A drag ends on release; a
    // change without one, as from the keyboard, rests after a short pause.
    auto *rest = new QTimer(session);
    rest->setSingleShot(true);
    rest->setInterval(SELECTION_REST_MS);
    const QPointer<CanvasScene> canvas(scene);
    const auto follow = [session, canvas] {
        if (canvas && session->shown() && !canvas->selection_model->is_dragging()) session->show(canvas->selection_model->rect());
    };
    QObject::connect(rest, &QTimer::timeout, session, follow);
    QObject::connect(scene->selection_model, &SelectionModel::rectChanged, session, [session, rest](const QRectF &rect) {
        if (!session->shown() || session->region_of(rect) == session->region()) return;
        session->suspend();
        rest->start();
    });
    QObject::connect(scene->selection_model, &SelectionModel::draggingChanged, session, [rest, follow](bool dragging) {
        if (dragging) return;
        rest->stop();
        follow();
    });
    const auto report = [parent](const QString &message) {
        if (parent) show_modeless_warning_dialog(parent, action_tr("Translation"), message);
    };
    QObject::connect(session, &OverlayTranslationSession::failed, session, report);
    QObject::connect(session, &OverlayTranslationSession::partly_failed, session, report);
    QObject::connect(session, &OverlayTranslationSession::translations_did_not_fit, session, [parent](const QString &text, const QString &notice) {
        if (parent) show_unplaced_overlay_translations(parent, action_tr("Translation"), text, notice);
    });
    QObject::connect(session, &OverlayTranslationSession::nothing_to_translate, session,
                     [] { show_toast(action_tr("Nothing to translate: the text is already in the target language.")); });
    return session;
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
