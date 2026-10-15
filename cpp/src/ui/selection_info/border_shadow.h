#pragma once
#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <functional>

namespace jietuba {
class ColorPickerButton;
class ExportService;
class MaskOverlayWidget;
class RoundedCornersLogic;
class SelectionItem;
class SelectionModel;
class ToolSettingsManager;

// A colour block plus label acting as one mode choice; the whole group is clickable.
class BorderShadowGroup : public QWidget {
    Q_OBJECT
public:
    using QWidget::QWidget;
    std::function<void(QMouseEvent *)> on_press;
protected:
    void mousePressEvent(QMouseEvent *event) override;
};

// Border/shadow popup: shadow or border (one of them), a size slider, and a
// "keep enabled" switch. Clicking a group picks the mode; right-click or the
// colour block opens the colour picker.
class BorderShadowPopup : public QWidget {
    Q_OBJECT
public:
    static constexpr int BASE_RADIUS = 6;
    static constexpr int BASE_WIDTH = 200;
    static constexpr int BASE_COLOR_BLOCK = 18;
    static constexpr int BASE_VALUE_LABEL_WIDTH = 28;
    explicit BorderShadowPopup(QWidget *parent);
    // Sizes only; mode, colours and size value stay.
    void apply_scale();
    QString mode() const { return current_mode_; }
    void set_mode(const QString &mode);
    int size_value() const { return slider_->value(); }
    void set_size_value(int value) { slider_->setValue(value); }
    bool persist() const { return chk_persist_->isChecked(); }
    void set_persist(bool value) { chk_persist_->setChecked(value); }
    QString shadow_color() const { return shadow_color_; }
    void set_shadow_color(const QString &color);
    QString border_color() const { return border_color_; }
    void set_border_color(const QString &color);
    void schedule_hide() { hide_timer_->start(); }
    void cancel_hide() { hide_timer_->stop(); }
    // Above the anchor when there is room, else below.
    void show_near(QWidget *anchor);
signals:
    void mode_changed(const QString &mode);  // "shadow" / "border"
    void size_changed(int size);
    void shadow_color_changed(const QString &color_hex);
    void border_color_changed(const QString &color_hex);
    void persist_changed(bool persist);
    void visibility_changed(bool visible);
protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
private:
    void init_ui();
    // The selected group gets a highlighted background.
    void refresh_mode_buttons();
    void select_mode(const QString &mode);
    void do_hide();
    QString current_mode_ = "shadow";
    QString shadow_color_ = "#0078FF";
    QString border_color_ = "#FF0000";
    QVBoxLayout *root_layout_ = nullptr;
    QHBoxLayout *shadow_row_ = nullptr, *border_row_ = nullptr, *mode_row_ = nullptr, *slider_row_ = nullptr;
    BorderShadowGroup *shadow_group_ = nullptr, *border_group_ = nullptr;
    ColorPickerButton *shadow_color_block_ = nullptr, *border_color_block_ = nullptr;
    QLabel *shadow_label_ = nullptr, *border_label_ = nullptr, *size_label_ = nullptr, *label_val_ = nullptr;
    QSlider *slider_ = nullptr;
    QCheckBox *chk_persist_ = nullptr;
    QTimer *hide_timer_ = nullptr;
    QPointer<QWidget> anchor_;
};

// Border/shadow: popup handling, a mask-overlay preview while the popup is
// visible, and the effect added around the exported image; follows the
// rounded-corner radius when that is enabled.
class BorderShadowLogic : public QObject {
    Q_OBJECT
public:
    BorderShadowLogic(QWidget *parent_widget, QPushButton *btn_border, SelectionItem *selection_item, MaskOverlayWidget *mask_overlay,
                      ExportService *export_service, SelectionModel *selection_model, ToolSettingsManager *config_manager = nullptr,
                      RoundedCornersLogic *rounded_corners_logic = nullptr);
    // Late binding, for construction order.
    void set_rounded_corners_logic(RoundedCornersLogic *logic) { rounded_logic_ = logic; }
    void set_enabled(bool enabled);
    bool enabled() const { return enabled_; }
    void uninstall();
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    // The rounded-corner radius, or 0 when rounding is off.
    int corner_radius() const;
    QString active_color() const;
    void on_mode_changed(const QString &mode);
    void on_size_changed(int value);
    void on_shadow_color_changed(const QString &color_hex);
    void on_border_color_changed(const QString &color_hex);
    void on_persist_changed(bool checked);
    // Preview only while the popup is visible.
    void on_popup_visibility_changed(bool visible);
    void paint_preview();
    QPointer<QPushButton> btn_;
    QPointer<MaskOverlayWidget> mask_;
    ExportService *export_;
    SelectionModel *model_;
    ToolSettingsManager *config_;
    RoundedCornersLogic *rounded_logic_;
    bool enabled_ = false;
    QString mode_ = "border";
    int size_ = 21;
    QString shadow_color_ = "#0078FF";
    QString border_color_ = "#FF0000";
    bool persist_ = false;
    bool preview_visible_ = false;
    QPointer<BorderShadowPopup> popup_;
    int mask_hook_ = 0, export_hook_ = 0;
};

// Stroke outside the selection edge, following the corner radius.
void draw_border_preview(QPainter &painter, const QRect &local_sel, int size, const QColor &color, int corner_radius = 0);
// Layered translucent outlines approximating a soft shadow.
void draw_shadow_preview(QPainter &painter, const QRect &local_sel, int size, const QColor &color, int corner_radius = 0);
QImage apply_border_export(const QImage &img, int size, const QColor &color, int corner_radius = 0);
QImage apply_shadow_export(const QImage &img, int size, const QColor &color, int corner_radius = 0);
}
