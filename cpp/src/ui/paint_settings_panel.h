#pragma once
#include "ui/base_settings_panel.h"
#include <QButtonGroup>
#include <QGraphicsItem>

namespace jietuba {
// Panel for pen and highlighter; the spotlight borrows it for darkness only.
class PaintSettingsPanel : public BaseSettingsPanel, public LineStylePanel {
    Q_OBJECT
public:
    static constexpr int BASE_COMBO_WIDTH = 88;
    static constexpr int BASE_COMBO_ICON_H = 16;
    static constexpr int BASE_MODE_BTN = 27;
    static constexpr int BASE_MODE_ICON = 22;
    explicit PaintSettingsPanel(QWidget *parent = nullptr);
    void init_ui() override;
    QString line_style() const override { return current_line_style; }
    void set_line_style(const QString &value) override;
    void set_state_from_item(QGraphicsItem *item);
    void set_line_style_visible(bool visible);
    void set_size_visible(bool visible);
    void set_color_visible(bool visible);
    void set_highlighter_mode_visible(bool visible);
    void set_highlighter_mode(const QString &mode);
    QString current_line_style = "solid";
    QString current_highlighter_mode = "freehand";
    QWidget *mode_widget = nullptr;
    QPushButton *freehand_btn = nullptr;
    QPushButton *rect_btn = nullptr;
    QButtonGroup *mode_group = nullptr;
    PillComboBox *line_style_combo = nullptr;
    QWidget *color_widget = nullptr;
signals:
    void line_style_changed(const QString &style);
    void highlighter_mode_changed(const QString &mode);
protected:
    QString build_stylesheet() const override;
    void apply_scale_sizes() override;
private:
    void on_line_style_changed();
    void on_highlighter_mode_clicked();
    // Panel width follows the visible controls, leaving no gap for hidden ones.
    void set_control_visible(QWidget *widget, bool visible);
    QHBoxLayout *color_layout_ = nullptr;
};

// Populates a line-style combo with the three styles (icon when the SVG exists, text otherwise).
void add_line_style_items(QComboBox *combo);
QString line_style_from_combo(QComboBox *combo);
}
