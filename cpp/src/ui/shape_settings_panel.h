#pragma once
#include "ui/base_settings_panel.h"

class QGraphicsItem;

namespace jietuba {
// Panel for rectangle and ellipse.
class ShapeSettingsPanel : public BaseSettingsPanel, public LineStylePanel {
    Q_OBJECT
public:
    static constexpr int BASE_COMBO_WIDTH = 88;
    static constexpr int BASE_COMBO_ICON_H = 16;
    explicit ShapeSettingsPanel(QWidget *parent = nullptr);
    void init_ui() override;
    QString line_style() const override { return current_line_style; }
    void set_line_style(const QString &value) override;
    void set_state_from_item(QGraphicsItem *item);
    QString current_line_style = "solid";
    PillComboBox *line_style_combo = nullptr;
signals:
    void line_style_changed(const QString &style);
protected:
    std::pair<int, int> size_range() const override { return {1, 40}; }
    int size_default() const override { return 3; }
    QString build_stylesheet() const override;
    void apply_scale_sizes() override;
};
}
