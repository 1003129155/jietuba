#pragma once
#include "canvas/items/watermark_item.h"
#include "ui/base_settings_panel.h"
#include <QMap>
#include <QSlider>

namespace jietuba {
class ColorPickerButton;

// The watermark text on the panel: shows it as applied, click to edit, × clears it.
class WatermarkTextField : public QWidget {
    Q_OBJECT
public:
    static constexpr int BASE_WIDTH = 168;
    static constexpr int BASE_HEIGHT = 26;
    static constexpr int BASE_PADDING = 12;
    static constexpr int BASE_CLEAR = 16;
    explicit WatermarkTextField(QWidget *parent = nullptr);
    void apply_scale();
    // when is the time of the watermark on the image; expanded once here so repaints
    // show the same text as the image rather than the current time.
    void set_template(const QString &template_text, const QDateTime &when = QDateTime());
    QString template_text() const { return template_; }
    QString text() const { return text_; }
    void retranslate();
signals:
    void clicked();
    void cleared();
protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
private:
    QRectF clear_rect() const;
    QString template_, text_;
};

// Opens over the style button: opacity, size, spacing and angle sliders, then a colour row.
class WatermarkStylePopup : public HoverPopup {
    Q_OBJECT
public:
    static constexpr int BASE_LABEL_WIDTH = 56;
    static constexpr int BASE_VALUE_WIDTH = 36;
    static constexpr int BASE_SLIDER_WIDTH = 150;
    explicit WatermarkStylePopup(QWidget *parent = nullptr);
    void apply_scale() override;
    void load_style(const WatermarkStyle &style);
    void retranslate();
    ColorPickerButton *color_button = nullptr;
    ColorRow *color_row = nullptr;
signals:
    // (WatermarkStyle field name, new value)
    void changed(const QString &key, const QVariant &value);
private:
    static QString controls_qss();
    void show_value(const QString &key, int value);
    void on_slider(const QString &key, int value);
    void on_preset(const QString &color_hex);
    QMap<QString, QSlider *> sliders_;
    QMap<QString, QLabel *> values_;
    QMap<QString, QLabel *> labels_;
};

class WatermarkSettingsPanel : public QWidget {
    Q_OBJECT
public:
    static constexpr int BASE_MARGIN_H = 12;
    static constexpr int BASE_MARGIN_V = 6;
    static constexpr int BASE_SPACING = 10;
    static constexpr int BASE_BTN = 28;
    static constexpr int BASE_ICON = 20;
    static constexpr int BASE_LAYOUT_COMBO_MIN_WIDTH = 96;
    static constexpr int BASE_FONT_COMBO_WIDTH = 132;
    explicit WatermarkSettingsPanel(QWidget *parent = nullptr);
    void apply_scale();
    void show_style_popup();
    // Setters for the toolbar; they do not emit.
    void load_style(const WatermarkStyle &style);
    void set_template(const QString &template_text, const QDateTime &when = QDateTime());
    void retranslate();
    WatermarkTextField *text_field = nullptr;
    PillComboBox *layout_combo = nullptr;
    QPushButton *style_button = nullptr;
    PillComboBox *font_combo = nullptr;
signals:
    void style_changed(const QString &key, const QVariant &value);
    void text_edit_requested();
    void text_cleared();
protected:
    void paintEvent(QPaintEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    // Position names differ a lot in length across languages; sized to the longest, never elided.
    void fit_layout_combo();
    void fill_fonts();
    void on_popup_changed(const QString &key, const QVariant &value);
    void on_layout_changed();
    void on_font_changed();
    WatermarkStyle style_;
    WatermarkStylePopup *style_popup_ = nullptr;
};
}
