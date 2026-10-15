#include "ui/paint_settings_panel.h"
#include "canvas/items/stroke_item.h"
#include "core/resource_manager.h"
#include "core/ui_scale.h"
#include "ui/color_picker_button.h"
#include <QFile>
#include <QGraphicsItem>

namespace jietuba {
namespace {
QIcon cached_icon(const QString &svg_name) { return ResourceManager::get_icon(ResourceManager::get_icon_path(svg_name)); }
}

void add_line_style_items(QComboBox *combo) {
    const std::pair<const char *, std::pair<const char *, const char *>> items[] = {
        {"solid", {"line_solid.svg", "──"}}, {"dashed", {"line_dash.svg", "- - -"}}, {"dashed_dense", {"line_dot.svg", "- -"}},
    };
    for (const auto &[data, icon_text] : items) {
        if (QFile::exists(ResourceManager::get_icon_path(icon_text.first))) combo->addItem(cached_icon(icon_text.first), "", data);
        else combo->addItem(QString::fromUtf8(icon_text.second), data);
    }
}

QString line_style_from_combo(QComboBox *combo) {
    auto style = combo->currentData().toString();
    if (style.isEmpty()) {
        // Without data, the fallback text identifies the style.
        style = combo->currentText();
        if (style == QString::fromUtf8("──")) style = "solid";
        else if (style == "- - -") style = "dashed";
        else if (style == "- -") style = "dashed_dense";
    }
    return style;
}

PaintSettingsPanel::PaintSettingsPanel(QWidget *parent) : BaseSettingsPanel(parent) { init_ui(); }

QString PaintSettingsPanel::build_stylesheet() const {
    return build_settings_panel_stylesheet(true, "1px", BASE_COMBO_WIDTH, BASE_COMBO_WIDTH, true);
}

void PaintSettingsPanel::apply_scale_sizes() {
    BaseSettingsPanel::apply_scale_sizes();
    const int mode_btn = scaled(BASE_MODE_BTN), mode_icon = scaled(BASE_MODE_ICON);
    for (auto *btn : {freehand_btn, rect_btn}) {
        btn->setFixedSize(mode_btn, mode_btn);
        btn->setIconSize(QSize(mode_icon, mode_icon));
    }
    mode_widget->layout()->setSpacing(scaled(2));
    line_style_combo->setIconSize(QSize(scaled(BASE_COMBO_WIDTH), scaled(BASE_COMBO_ICON_H)));
    color_layout_->setSpacing(scaled(BASE_SPACING));
}

void PaintSettingsPanel::init_ui() {
    current_size = size_default();
    auto *layout = new QHBoxLayout(this);
    // Highlighter mode switch.
    mode_widget = new QWidget;
    auto *mode_layout = new QHBoxLayout(mode_widget);
    mode_layout->setContentsMargins(0, 0, 0, 0);
    freehand_btn = new QPushButton;
    freehand_btn->setCheckable(true);
    freehand_btn->setToolTip(tr_panel("Freehand Highlight"));
    freehand_btn->setIcon(cached_icon(QString::fromUtf8("画笔.svg")));
    freehand_btn->setStyleSheet("QPushButton { padding: 0px; }");
    rect_btn = new QPushButton;
    rect_btn->setCheckable(true);
    rect_btn->setToolTip(tr_panel("Rect Highlight"));
    rect_btn->setIcon(cached_icon(QString::fromUtf8("方框.svg")));
    rect_btn->setStyleSheet("QPushButton { padding: 0px; }");
    mode_group = new QButtonGroup(this);
    mode_group->setExclusive(true);
    mode_group->addButton(freehand_btn);
    mode_group->addButton(rect_btn);
    mode_layout->addWidget(freehand_btn);
    mode_layout->addWidget(rect_btn);
    layout->addWidget(mode_widget);
    // Line style.
    line_style_combo = new PillComboBox;
    line_style_combo->set_preview_painter(&paint_line_style_preview);
    add_line_style_items(line_style_combo);
    line_style_combo->setToolTip(tr_panel("Line Style"));
    line_style_combo->setMaxVisibleItems(10);
    layout->addWidget(line_style_combo);
    build_size_and_opacity(layout);
    // Separator and colours as one group, hidden together when the spotlight borrows the panel.
    color_widget = new QWidget;
    color_layout_ = new QHBoxLayout(color_widget);
    color_layout_->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(color_widget);
    color_layout_->addWidget(make_separator());
    color_layout_->addWidget(build_color_row());
    layout->addStretch();
    connect(mode_group, &QButtonGroup::buttonClicked, this, [this](QAbstractButton *) { on_highlighter_mode_clicked(); });
    connect(line_style_combo, &QComboBox::currentIndexChanged, this, [this](int) { on_line_style_changed(); });
    connect(size_spin, &StepperWidget::valueChanged, this, &PaintSettingsPanel::on_size_changed);
    connect(opacity_spin, &StepperWidget::valueChanged, this, &PaintSettingsPanel::on_opacity_changed);
    connect(color_picker_btn, &ColorPickerButton::color_changed, this, &PaintSettingsPanel::on_color_picked);
    build_extra_controls(layout);
    apply_scale();
    set_highlighter_mode(current_highlighter_mode);
}

void PaintSettingsPanel::on_line_style_changed() {
    const auto style = line_style_from_combo(line_style_combo);
    current_line_style = style;
    emit line_style_changed(style);
}

void PaintSettingsPanel::set_line_style(const QString &value) {
    if (value != "solid" && value != "dashed" && value != "dashed_dense") return;
    current_line_style = value;
    const int idx = line_style_combo->findData(value);
    if (idx >= 0) {
        line_style_combo->blockSignals(true);
        line_style_combo->setCurrentIndex(idx);
        line_style_combo->blockSignals(false);
    }
}

void PaintSettingsPanel::set_state_from_item(QGraphicsItem *item) {
    auto *shape = dynamic_cast<QAbstractGraphicsShapeItem *>(item);
    if (!item) return;
    blockSignals(true);
    if (shape) {
        const auto pen = shape->pen();
        set_line_style(line_style_of_pen(pen));
        double width = pen.widthF();
        // A highlighter stroke is drawn three times wider than its logical width.
        if (auto *stroke = dynamic_cast<StrokeItem *>(item); stroke && stroke->is_highlighter)
            if (const auto logical = stroke->get_stroke_width()) width = *logical;
        if (auto *rect = dynamic_cast<RectItem *>(item); rect && rect->is_highlighter)
            if (const auto logical = rect->get_stroke_width()) width = *logical;
        size_spin->setValue(static_cast<int>(python_round(width)));
        const auto color = pen.color();
        current_color = color;
        color_picker_btn->set_color(color);
        current_opacity = color.alpha();
        opacity_spin->setValue(opacity_to_percent(current_opacity));
    }
    blockSignals(false);
}

void PaintSettingsPanel::set_control_visible(QWidget *widget, bool visible) {
    if (!widget->isHidden() == visible) return;
    widget->setVisible(visible);
    adjustSize();
}

void PaintSettingsPanel::set_line_style_visible(bool visible) { set_control_visible(line_style_combo, visible); }
void PaintSettingsPanel::set_size_visible(bool visible) { set_control_visible(size_spin, visible); }
void PaintSettingsPanel::set_color_visible(bool visible) { set_control_visible(color_widget, visible); }
void PaintSettingsPanel::set_highlighter_mode_visible(bool visible) { set_control_visible(mode_widget, visible); }

void PaintSettingsPanel::set_highlighter_mode(const QString &requested) {
    const QString mode = requested == "freehand" || requested == "rect" ? requested : QString("freehand");
    current_highlighter_mode = mode;
    if (freehand_btn && rect_btn) {
        freehand_btn->blockSignals(true);
        rect_btn->blockSignals(true);
        freehand_btn->setChecked(mode == "freehand");
        rect_btn->setChecked(mode == "rect");
        freehand_btn->blockSignals(false);
        rect_btn->blockSignals(false);
    }
}

void PaintSettingsPanel::on_highlighter_mode_clicked() {
    const QString mode = freehand_btn->isChecked() ? "freehand" : "rect";
    current_highlighter_mode = mode;
    emit highlighter_mode_changed(mode);
}
}
