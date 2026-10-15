#pragma once
#include <QImage>
#include <QPointer>
#include <QRectF>
#include <QTimer>
#include <QWidget>
#include <memory>

namespace jietuba {
class BorderShadowLogic;
class ExportService;
class LockRatioLogic;
class MaskOverlayWidget;
class RoundedCornersLogic;
class SelectionInfoPanel;
class SelectionItem;
class SelectionModel;
class SystemCursor;
class ToolSettingsManager;

// What the capture window offers the background refresh, in place of the
// reference's attribute lookups on the window.
class SelectionInfoHost {
public:
    virtual ~SelectionInfoHost() = default;
    // window._is_closing
    virtual bool selection_info_closing() const = 0;
    // window._exclude_from_capture_set and window._set_exclude_from_capture
    bool exclude_from_capture_set = false;
    virtual void set_exclude_from_capture(bool exclude) = 0;
    // window._capture_cursor, recorded when the session started.
    virtual std::shared_ptr<SystemCursor> capture_cursor() const = 0;
    // window.original_image = image; window.scene.background.update_image(image)
    virtual void replace_background(const QImage &image) = 0;
};

// Drives the info panel from the selection model and delegates its buttons to
// the lock-ratio, rounded-corner and border/shadow modules.
class SelectionInfoController {
public:
    SelectionInfoController(SelectionInfoPanel *panel, SelectionModel *selection_model, SelectionItem *selection_item, ExportService *export_service,
                            MaskOverlayWidget *mask_overlay, QWidget *parent_widget, ToolSettingsManager *config_manager = nullptr);
    ~SelectionInfoController();
    // Removes every hook, stops the timers and restores the original behaviour.
    void cleanup();
    // One refresh: makes the capture window invisible to screen capture on first
    // use, then captures the screens and replaces the background.
    void do_refresh_background();
    SelectionInfoPanel *panel;
    SelectionModel *model;
    SelectionItem *selection_item;
    ExportService *export_service;
    MaskOverlayWidget *mask_overlay;
private:
    void connect_signals();
    void on_rect_changed(const QRectF &rect);
    void on_dragging_changed(bool is_dragging);
    void on_confirmed(const QRectF &rect);
    void on_lock_ratio_toggled(bool checked);
    // Pressing refreshes once at once and starts long-press detection.
    void on_refresh_pressed();
    void on_refresh_released();
    // After a 300 ms hold: continuous refresh.
    void start_continuous_refresh();
    ToolSettingsManager *config_;
    std::unique_ptr<LockRatioLogic> lock_ratio_;
    QPointer<RoundedCornersLogic> rounded_corners_;
    QPointer<BorderShadowLogic> border_shadow_;
    QPointer<QWidget> parent_window_;
    QTimer refresh_timer_;
    QTimer long_press_timer_;
    bool refresh_pending_ = false;
    QList<QMetaObject::Connection> model_connections_;  // model and panel connections
    bool cleaned_up_ = false;
};
}
