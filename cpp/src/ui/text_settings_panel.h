#pragma once
#include "core/i18n.h"
#include "ui/base_settings_panel.h"
#include <QFont>
#include <QMap>
#include <QSlider>
#include <QVBoxLayout>

class QGraphicsTextItem;

namespace jietuba {
// Renders a real TextItem as the icon, so outline width and shadow distance use
// the drawing code itself. ratio is the device pixel ratio for crisp icons.
QPixmap render_text_effect_icon(int side, double ratio = 1.0, std::optional<double> outline_width = std::nullopt, bool shadow = false);

// Standard slider with dots on the four steps, so discrete options do not look continuous.
class FourStepSlider : public QSlider {
public:
    using QSlider::QSlider;
protected:
    void paintEvent(QPaintEvent *event) override;
};

// Text tool panel: font, size, bold/italic/underline, background/outline/shadow
// toggles with hover popups, and the text colour.
class TextSettingsPanel : public QWidget {
    Q_OBJECT
public:
    // Q_OBJECT's tr would use the namespaced class name; translations use the Python class name.
    static QString tr_text(const char *text) { return i18n_tr(text, "TextSettingsPanel"); }
    static constexpr int BASE_MARGIN_H = 12;
    static constexpr int BASE_MARGIN_V = 6;
    static constexpr int BASE_SPACING = 10;
    static constexpr int BASE_SIZE_SPIN_WIDTH = 54;
    static constexpr int BASE_BTN = 28;
    static constexpr int BASE_EFFECT_ICON = 18;
    static constexpr int BASE_FONT_VIEW_WIDTH = 200;
    static constexpr int BASE_OPACITY_LABEL_WIDTH = 36;
    static constexpr int BASE_OUTLINE_SLIDER_WIDTH = 108;
    explicit TextSettingsPanel(QWidget *parent = nullptr);
    void apply_scale();
    void set_background_settings(bool enabled, const QColor &color, int opacity);
    void set_outline_settings(bool enabled, const QColor &color, const QVariant &width);
    void set_shadow_settings(bool enabled, const QColor &color);
    void set_state_from_item(QGraphicsTextItem *item);
    // Refreshes tooltips after a language switch; the panel lives across captures.
    void retranslate();
    void load_from_config();
    static void save_font_to_config(const QFont &font);
    static void save_background_to_config(bool enabled, const QColor &color, int opacity);
    static void save_outline_to_config(bool enabled, const QColor &color, double width);
    // Colour with alpha stored as #AARRGGBB.
    static void save_shadow_to_config(bool enabled, const QColor &color);

    QFont current_font;
    QColor current_color = QColor(Qt::red);
    bool background_enabled = false;
    QColor background_color = QColor(255, 255, 255);
    int background_opacity = 255;
    bool outline_enabled = false;
    QColor outline_color;
    double outline_width;
    bool shadow_enabled = false;
    QColor shadow_color;
    PillComboBox *font_combo = nullptr;
    StepperWidget *size_spin = nullptr;
    QPushButton *bold_btn = nullptr, *italic_btn = nullptr, *underline_btn = nullptr;
    QPushButton *background_btn = nullptr, *outline_btn = nullptr, *shadow_btn = nullptr;
    ColorPickerButton *color_btn = nullptr;
    ColorPickerButton *background_color_btn = nullptr, *outline_color_btn = nullptr, *shadow_color_btn = nullptr;
    QSlider *background_opacity_slider = nullptr, *shadow_opacity_slider = nullptr;
    QLabel *background_opacity_label = nullptr, *shadow_opacity_label = nullptr;
    FourStepSlider *outline_width_slider = nullptr;
signals:
    void font_changed(const QFont &font);
    void color_changed(const QColor &color);
    void background_changed(bool enabled, const QColor &color, int opacity);
    // width is one of TextItem::OUTLINE_WIDTH_LEVELS.
    void outline_changed(bool enabled, const QColor &color, double width);
    // The colour's alpha is the shadow opacity.
    void shadow_changed(bool enabled, const QColor &color);
protected:
    void paintEvent(QPaintEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    static QString popup_controls_qss();
    QString build_stylesheet() const;
    void apply_popup_scale();
    void init_ui();
    void init_effect_popups();
    // Shrinks the top-level popup to its contents instead of the 200 px floor adjustSize applies.
    static void fit_effect_popup_width(HoverPopup *popup);
    std::pair<HoverPopup *, ColorPickerButton *> build_effect_popup(QPushButton *toggle, const QString &custom_tip, std::function<void(const QColor &)> on_color);
    std::pair<QSlider *, QLabel *> add_opacity_row(HoverPopup *popup, const QString &tip, std::function<void(int)> on_change);
    FourStepSlider *add_outline_width_slider(HoverPopup *popup);
    void connect_signals();
    void sync_effect_popup(QPushButton *toggle, bool checked);
    static void refresh_opacity_label(QLabel *label, int alpha);
    void update_background_btn_style();
    void emit_background_changed();
    void on_outline_width_changed(int index);
    void update_outline_width_slider_tooltip(std::optional<int> index = std::nullopt);
    void emit_outline_changed();
    void emit_shadow_changed();
    void on_font_changed();
    QStringList cached_fonts_;
    QMap<QPushButton *, HoverPopup *> effect_popups_;
    std::vector<std::pair<QPushButton *, QString>> font_style_buttons_;
    ColorRow *color_row_ = nullptr;
    QList<QVBoxLayout *> popup_layouts_;
    QList<ColorRow *> popup_color_rows_;
    QList<QLabel *> opacity_labels_;
    QList<QLabel *> outline_width_previews_;
    QHBoxLayout *outline_row_ = nullptr;
};
}
