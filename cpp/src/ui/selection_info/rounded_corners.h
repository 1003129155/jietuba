#pragma once
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QWidget>

namespace jietuba {
class ExportService;
class MaskOverlayWidget;
class SelectionItem;
class SelectionModel;
class ToolSettingsManager;

// Floating radius slider. Shown while the pointer is on the rounded-corner button
// or on the popup; hidden shortly after it leaves both.
class RoundedSliderPopup : public QWidget {
    Q_OBJECT
public:
    static constexpr int BASE_RADIUS = 6;
    static constexpr int BASE_HEIGHT = 30;
    static constexpr int BASE_WIDTH = 180;
    static constexpr int BASE_SLIDER_WIDTH = 110;
    static constexpr int BASE_LABEL_WIDTH = 30;
    explicit RoundedSliderPopup(QWidget *parent);
    // Sizes only; the radius stays.
    void apply_scale();
    int value() const { return slider_->value(); }
    void set_value(int value) { slider_->setValue(value); }
    // Delayed, leaving time to move onto the popup.
    void schedule_hide() { hide_timer_->start(); }
    void cancel_hide() { hide_timer_->stop(); }
    // Above the anchor when there is room, else below.
    void show_near(QWidget *anchor);
signals:
    void radius_changed(int radius);
protected:
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
private:
    void init_ui();
    void do_hide();
    QHBoxLayout *row_layout_ = nullptr;
    QSlider *slider_ = nullptr;
    QLabel *label_ = nullptr;
    QTimer *hide_timer_ = nullptr;
    QPointer<QWidget> anchor_;
};

// Rounded-corner capture: the slider popup, the rounded selection frame, mask
// fill at the four corners for preview, and transparent corners on export.
class RoundedCornersLogic : public QObject {
    Q_OBJECT
public:
    RoundedCornersLogic(QWidget *parent_widget, QPushButton *btn_rounded, SelectionItem *selection_item, MaskOverlayWidget *mask_overlay,
                        ExportService *export_service, SelectionModel *selection_model, ToolSettingsManager *config_manager = nullptr);
    void set_enabled(bool enabled);
    bool enabled() const { return enabled_; }
    int radius() const { return radius_; }
    void uninstall();
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    void on_radius_changed(int value);
    void install_render_hook();
    void install_mask_hook();
    void install_export_hook();
    void paint_mask_corners();
    QPointer<QPushButton> btn_;
    SelectionItem *item_;
    QPointer<MaskOverlayWidget> mask_;
    ExportService *export_;
    SelectionModel *model_;
    ToolSettingsManager *config_;
    bool enabled_ = false;
    int radius_ = 16;
    QPointer<RoundedSliderPopup> popup_;
    int mask_hook_ = 0, export_hook_ = 0;
    bool render_installed_ = false;
};
}
