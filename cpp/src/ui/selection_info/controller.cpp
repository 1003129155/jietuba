#include "ui/selection_info/controller.h"
#include "canvas/selection_model.h"
#include "capture/capture_service.h"
#include "core/logger.h"
#include "settings/tool_settings.h"
#include "ui/selection_info/border_shadow.h"
#include "ui/selection_info/lock_ratio.h"
#include "ui/selection_info/panel.h"
#include "ui/selection_info/rounded_corners.h"

namespace jietuba {
SelectionInfoController::SelectionInfoController(SelectionInfoPanel *panel, SelectionModel *selection_model, SelectionItem *selection_item,
                                                 ExportService *export_service, MaskOverlayWidget *mask_overlay, QWidget *parent_widget,
                                                 ToolSettingsManager *config_manager)
    : panel(panel), model(selection_model), selection_item(selection_item), export_service(export_service), mask_overlay(mask_overlay), config_(config_manager),
      parent_window_(parent_widget) {
    lock_ratio_ = std::make_unique<LockRatioLogic>(selection_item);
    rounded_corners_ = new RoundedCornersLogic(parent_widget, panel->btn_rounded, selection_item, mask_overlay, export_service, selection_model, config_manager);
    border_shadow_ = new BorderShadowLogic(parent_widget, panel->btn_border, selection_item, mask_overlay, export_service, selection_model, config_manager,
                                           rounded_corners_);
    // One full virtual-desktop capture takes about 20 ms (24 ms peak); 16 ms would fire
    // back to back and starve the main thread, 30 ms leaves the event loop a gap.
    refresh_timer_.setInterval(30);
    QObject::connect(&refresh_timer_, &QTimer::timeout, &refresh_timer_, [this] { do_refresh_background(); });
    long_press_timer_.setSingleShot(true);
    long_press_timer_.setInterval(300);
    QObject::connect(&long_press_timer_, &QTimer::timeout, &long_press_timer_, [this] { start_continuous_refresh(); });
    connect_signals();
}

SelectionInfoController::~SelectionInfoController() { cleanup(); }

void SelectionInfoController::connect_signals() {
    model_connections_.append(QObject::connect(model, &SelectionModel::rectChanged, panel, [this](const QRectF &rect) { on_rect_changed(rect); }));
    model_connections_.append(QObject::connect(model, &SelectionModel::draggingChanged, panel, [this](bool dragging) { on_dragging_changed(dragging); }));
    model_connections_.append(QObject::connect(model, &SelectionModel::confirmed, panel, [this](const QRectF &rect) { on_confirmed(rect); }));
    model_connections_.append(QObject::connect(panel, &SelectionInfoPanel::border_toggled, panel, [this](bool checked) {
        if (border_shadow_) border_shadow_->set_enabled(checked);
    }));
    model_connections_.append(QObject::connect(panel, &SelectionInfoPanel::rounded_toggled, panel, [this](bool checked) {
        if (rounded_corners_) rounded_corners_->set_enabled(checked);
    }));
    model_connections_.append(QObject::connect(panel, &SelectionInfoPanel::lock_ratio_toggled, panel, [this](bool checked) { on_lock_ratio_toggled(checked); }));
    model_connections_.append(QObject::connect(panel, &SelectionInfoPanel::refresh_pressed, panel, [this] { on_refresh_pressed(); }));
    model_connections_.append(QObject::connect(panel, &SelectionInfoPanel::refresh_released, panel, [this] { on_refresh_released(); }));
}

void SelectionInfoController::on_rect_changed(const QRectF &rect) {
    if (rect.isEmpty()) {
        panel->hide();
        return;
    }
    panel->update_info_text(rect);
    panel->follow_rect(rect);
    // Before confirmation (smart selection hover) only the coordinates show.
    if (!model->is_confirmed()) panel->set_confirmed(false);
    if (!panel->isVisible() && !model->is_dragging()) {
        panel->show();
        panel->raise();
    }
}

void SelectionInfoController::on_dragging_changed(bool is_dragging) {
    if (is_dragging) {
        // Hiding while dragging is a setting; otherwise the panel follows the selection.
        if (config_->get_screenshot_info_hide_on_drag()) {
            panel->hide();
        } else if (!model->rect().isEmpty()) {
            panel->update_info_text(model->rect());
            panel->follow_rect(model->rect());
        }
    } else if (!model->rect().isEmpty()) {
        panel->update_info_text(model->rect());
        panel->follow_rect(model->rect());
        panel->show();
        panel->raise();
    }
}

void SelectionInfoController::on_confirmed(const QRectF &rect) {
    // Confirmed: the panel with all its action buttons.
    if (!rect.isEmpty()) {
        panel->update_info_text(rect);
        panel->follow_rect(rect);
        panel->set_confirmed(true);
        panel->show();
        panel->raise();
    }
}

void SelectionInfoController::on_lock_ratio_toggled(bool checked) {
    lock_ratio_->set_enabled(checked, checked ? std::optional<QRectF>(model->rect()) : std::nullopt);
}

void SelectionInfoController::on_refresh_pressed() {
    refresh_pending_ = true;
    do_refresh_background();
    long_press_timer_.start();
}

void SelectionInfoController::on_refresh_released() {
    long_press_timer_.stop();
    refresh_timer_.stop();
    refresh_pending_ = false;
}

void SelectionInfoController::start_continuous_refresh() {
    refresh_pending_ = false;
    refresh_timer_.start();
}

void SelectionInfoController::do_refresh_background() {
    try {
        auto *host = dynamic_cast<SelectionInfoHost *>(parent_window_.data());
        // Not while the window is closed or closing.
        if (!host || host->selection_info_closing()) {
            refresh_timer_.stop();
            long_press_timer_.stop();
            return;
        }
        // From the first refresh on, the capture window stays invisible to screen capture.
        if (!host->exclude_from_capture_set) {
            host->set_exclude_from_capture(true);
            host->exclude_from_capture_set = true;
        }
        // The pointer is on the refresh button now, so the session's recorded pointer is drawn.
        auto [new_image, rect] = CaptureService().capture_all_screens(host->capture_cursor());
        host->replace_background(new_image);
    } catch (const std::exception &e) {
        log_debug(T("刷新背景失败: {e}", {{"e", QString::fromUtf8(e.what())}}));
    }
}

void SelectionInfoController::cleanup() {
    if (cleaned_up_) return;
    cleaned_up_ = true;
    // Timers first, so no callback reaches destroyed objects.
    long_press_timer_.stop();
    refresh_timer_.stop();
    // Queued signals must not reach this controller after cleanup; the panel may outlive it.
    for (const auto &connection : model_connections_) QObject::disconnect(connection);
    model_connections_.clear();
    lock_ratio_->uninstall();
    if (rounded_corners_) rounded_corners_->uninstall();
    if (border_shadow_) border_shadow_->uninstall();
    // Both logic objects are children of the reused capture window, which is never
    // destroyed; without deleting them they would pile up session after session.
    if (rounded_corners_) {
        rounded_corners_->setParent(nullptr);
        rounded_corners_->deleteLater();
    }
    if (border_shadow_) {
        border_shadow_->setParent(nullptr);
        border_shadow_->deleteLater();
    }
    parent_window_ = nullptr;
}
}
