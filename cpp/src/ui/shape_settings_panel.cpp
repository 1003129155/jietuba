#include "ui/shape_settings_panel.h"
#include "core/ui_scale.h"
#include "ui/color_picker_button.h"
#include "ui/paint_settings_panel.h"
#include <QAbstractGraphicsShapeItem>

namespace jietuba {
ShapeSettingsPanel::ShapeSettingsPanel(QWidget *parent) : BaseSettingsPanel(parent) { init_ui(); }

QString ShapeSettingsPanel::build_stylesheet() const {
    return build_settings_panel_stylesheet(true, "1px", BASE_COMBO_WIDTH, BASE_COMBO_WIDTH, true);
}

void ShapeSettingsPanel::apply_scale_sizes() {
    BaseSettingsPanel::apply_scale_sizes();
    line_style_combo->setIconSize(QSize(scaled(BASE_COMBO_WIDTH), scaled(BASE_COMBO_ICON_H)));
}

void ShapeSettingsPanel::init_ui() {
    current_size = size_default();
    auto *layout = new QHBoxLayout(this);
    line_style_combo = new PillComboBox;
    line_style_combo->set_preview_painter(&paint_line_style_preview);
    add_line_style_items(line_style_combo);
    line_style_combo->setToolTip(tr_panel("Line Style"));
    line_style_combo->setMaxVisibleItems(10);
    layout->addWidget(line_style_combo);
    build_size_and_opacity(layout);
    layout->addWidget(make_separator());
    layout->addWidget(build_color_row());
    layout->addStretch();
    connect(line_style_combo, &QComboBox::currentIndexChanged, this, [this](int) {
        current_line_style = line_style_from_combo(line_style_combo);
        emit line_style_changed(current_line_style);
    });
    connect(size_spin, &StepperWidget::valueChanged, this, &ShapeSettingsPanel::on_size_changed);
    connect(opacity_spin, &StepperWidget::valueChanged, this, &ShapeSettingsPanel::on_opacity_changed);
    connect(color_picker_btn, &ColorPickerButton::color_changed, this, &ShapeSettingsPanel::on_color_picked);
    build_extra_controls(layout);
    apply_scale();
}

void ShapeSettingsPanel::set_line_style(const QString &value) {
    if (value != "solid" && value != "dashed" && value != "dashed_dense") return;
    current_line_style = value;
    const int idx = line_style_combo->findData(value);
    if (idx >= 0) {
        line_style_combo->blockSignals(true);
        line_style_combo->setCurrentIndex(idx);
        line_style_combo->blockSignals(false);
    }
}

void ShapeSettingsPanel::set_state_from_item(QGraphicsItem *item) {
    if (!item) return;
    blockSignals(true);
    if (auto *shape = dynamic_cast<QAbstractGraphicsShapeItem *>(item)) {
        const auto pen = shape->pen();
        set_line_style(line_style_of_pen(pen));
        size_spin->setValue(static_cast<int>(python_round(pen.widthF())));
        const auto color = pen.color();
        current_color = color;
        color_picker_btn->set_color(color);
        current_opacity = color.alpha();
        opacity_spin->setValue(opacity_to_percent(current_opacity));
    }
    blockSignals(false);
}
}
