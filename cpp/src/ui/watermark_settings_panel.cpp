#include "ui/watermark_settings_panel.h"
#include "core/constants.h"
#include "core/i18n.h"
#include "core/resource_manager.h"
#include "core/ui_scale.h"
#include "ui/color_picker_button.h"
#include <QEvent>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QVBoxLayout>

namespace jietuba {
namespace {
QString wm_tr(const char *text) { return i18n_tr(text, "WatermarkPanel"); }

const char *layout_name(const QString &key) {
    static const QMap<QString, const char *> names = {
        {"tile", "Tiled"}, {"diamond", "Diamond"}, {"center", "Center"}, {"top_left", "Top Left"}, {"top", "Top Center"},
        {"top_right", "Top Right"}, {"bottom_left", "Bottom Left"}, {"bottom", "Bottom Center"}, {"bottom_right", "Bottom Right"},
    };
    return names.value(key, "");
}

const char *line_style_name(const QString &key) {
    static const QMap<QString, const char *> names = {{"none", "None"}, {"solid", "Solid"}, {"dash", "Dashed"}};
    return names.value(key, "");
}

struct SliderRow {
    const char *key;
    const char *text;
    int low, high;
    QVariant (*to_style)(int);
    int (*from_style)(const WatermarkStyle &);
};
// Field name, label, range, slider value <-> style value.
const SliderRow ROWS[] = {
    {"opacity", "Opacity", 0, 100, [](int v) { return QVariant(v / 100.0); }, [](const WatermarkStyle &s) { return static_cast<int>(python_round(s.opacity * 100)); }},
    {"font_size", "Size", MIN_FONT_SIZE, MAX_FONT_SIZE, [](int v) { return QVariant(v); }, [](const WatermarkStyle &s) { return s.font_size; }},
    {"spacing", "Spacing", MIN_SPACING, MAX_SPACING, [](int v) { return QVariant(v); }, [](const WatermarkStyle &s) { return s.spacing; }},
    {"angle", "Angle", MIN_ANGLE, MAX_ANGLE, [](int v) { return QVariant(v); }, [](const WatermarkStyle &s) { return s.angle; }},
};

QString value_suffix(const QString &key) {
    if (key == "opacity") return "%";
    if (key == "angle") return QString::fromUtf8("°");
    return {};
}
}

WatermarkTextField::WatermarkTextField(QWidget *parent) : QWidget(parent) {
    setAttribute(Qt::WA_Hover, true);
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
    apply_scale();
}

void WatermarkTextField::apply_scale() {
    setFixedSize(scaled(BASE_WIDTH), scaled(BASE_HEIGHT));
    update();
}

void WatermarkTextField::set_template(const QString &template_text, const QDateTime &when) {
    template_ = template_text;
    text_ = expand_text(template_, when.isValid() ? when : QDateTime::currentDateTime());
    setToolTip(text_.isEmpty() ? wm_tr("Input watermark") : text_);
    update();
}

QRectF WatermarkTextField::clear_rect() const {
    const double side = scaled_f(BASE_CLEAR);
    return QRectF(width() - scaled_f(5) - side, (height() - side) / 2, side, side);
}

void WatermarkTextField::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF rect(this->rect());
    painter.setPen(Qt::NoPen);
    painter.setBrush(underMouse() ? CONTROL_FILL_HOVER : CONTROL_FILL);
    painter.drawRoundedRect(rect, rect.height() / 2, rect.height() / 2);
    const auto clear = clear_rect();
    const double right = !template_.isEmpty() ? clear.left() - scaled_f(4) : rect.right() - scaled_f(BASE_PADDING);
    QRectF content(rect.left() + scaled_f(BASE_PADDING), rect.top(), 0, rect.height());
    content.setRight(right);
    // Multi-line watermarks show on one line separated by spaces; the tooltip has the full text.
    auto text = text_;
    text.replace("\n", "  ");
    painter.setFont(font());
    painter.setPen(!text.isEmpty() ? CONTROL_INK : CONTROL_SUB);
    const auto shown = fontMetrics().elidedText(!text.isEmpty() ? text : wm_tr("Input watermark"), Qt::ElideRight, static_cast<int>(content.width()));
    painter.drawText(content, Qt::AlignLeft | Qt::AlignVCenter, shown);
    if (!template_.isEmpty()) {
        const bool hovered = clear.contains(QPointF(mapFromGlobal(cursor().pos())));
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(CONTROL_SUB.red(), CONTROL_SUB.green(), CONTROL_SUB.blue(), hovered ? 200 : 120));
        painter.drawEllipse(clear);
        painter.setPen(QPen(QColor("white"), scaled_f(1.4), Qt::SolidLine, Qt::RoundCap));
        const auto c = clear.center();
        const double d = clear.width() * 0.2;
        painter.drawLine(QPointF(c.x() - d, c.y() - d), QPointF(c.x() + d, c.y() + d));
        painter.drawLine(QPointF(c.x() - d, c.y() + d), QPointF(c.x() + d, c.y() - d));
    }
    painter.end();
}

void WatermarkTextField::retranslate() {
    setToolTip(text_.isEmpty() ? wm_tr("Input watermark") : text_);
    update();
}

void WatermarkTextField::mouseMoveEvent(QMouseEvent *event) {
    update();
    QWidget::mouseMoveEvent(event);
}

void WatermarkTextField::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) {
        if (!template_.isEmpty() && clear_rect().contains(event->position())) emit cleared();
        else emit clicked();
    }
    QWidget::mouseReleaseEvent(event);
}

void WatermarkTextField::enterEvent(QEnterEvent *event) {
    update();
    QWidget::enterEvent(event);
}

void WatermarkTextField::leaveEvent(QEvent *event) {
    update();
    QWidget::leaveEvent(event);
}

WatermarkStylePopup::WatermarkStylePopup(QWidget *parent) : HoverPopup(parent) {
    auto *layout = new QVBoxLayout(this);
    for (const auto &row_def : ROWS) {
        const QString key = row_def.key;
        auto *label = new QLabel(wm_tr(row_def.text));
        auto *slider = new QSlider(Qt::Horizontal);
        slider->setRange(row_def.low, row_def.high);
        slider->setFocusPolicy(Qt::NoFocus);
        auto *value = new QLabel;
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        connect(slider, &QSlider::valueChanged, this, [this, key](int v) { on_slider(key, v); });
        auto *row = new QHBoxLayout;
        row->addWidget(label);
        row->addWidget(slider, 1);
        row->addWidget(value);
        layout->addLayout(row);
        sliders_[key] = slider;
        values_[key] = value;
        labels_[key] = label;
    }
    line_label_ = new QLabel(wm_tr("Grid Lines"));
    line_group_ = new QButtonGroup(this);
    auto *line_row = new QHBoxLayout;
    line_row->addWidget(line_label_);
    for (const auto &key : watermark_line_styles()) {
        auto *button = new QPushButton(wm_tr(line_style_name(key)));
        button->setObjectName("GridLineButton");
        button->setCheckable(true);
        button->setFocusPolicy(Qt::NoFocus);
        connect(button, &QPushButton::clicked, this, [this, key] { emit changed("line_style", key); });
        line_group_->addButton(button);
        line_row->addWidget(button, 1);
        line_buttons_[key] = button;
    }
    layout->addLayout(line_row);
    color_button = new ColorPickerButton(QColor(WatermarkStyle().color), false, 28, true, true);
    color_button->setToolTip(wm_tr("Custom Color"));
    connect(color_button, &ColorPickerButton::color_changed, this, [this](const QColor &color) { emit changed("color", color.name()); });
    color_row = new ColorRow(color_button, PRESET_COLORS, [this](const QString &color_hex) { on_preset(color_hex); });
    auto *colors = new QHBoxLayout;
    colors->addWidget(color_row);
    colors->addStretch();
    layout->addLayout(colors);
    apply_scale();
}

void WatermarkStylePopup::apply_scale() {
    extra_qss_ = controls_qss();
    HoverPopup::apply_scale();
    auto *layout = this->layout();
    if (!layout) return;
    const int margin = scaled(12);
    layout->setContentsMargins(margin, scaled(10), margin, scaled(10));
    layout->setSpacing(scaled(6));
    for (auto it = sliders_.begin(); it != sliders_.end(); ++it) {
        labels_[it.key()]->setFixedWidth(scaled(BASE_LABEL_WIDTH));
        values_[it.key()]->setFixedWidth(scaled(BASE_VALUE_WIDTH));
        it.value()->setFixedWidth(scaled(BASE_SLIDER_WIDTH));
    }
    if (line_label_) line_label_->setFixedWidth(scaled(BASE_LABEL_WIDTH));
    color_row->apply_scale();
    adjustSize();
}

QString WatermarkStylePopup::controls_qss() {
    const auto accent = accent_color().name();
    const QString groove = "rgba(15, 23, 42, 0.12)";
    const auto ink = CONTROL_INK.name();
    const QString buttons = QString(R"(
        QPushButton#GridLineButton { background: %1; color: %2; border: none; border-radius: %3px; padding: %4px %5px; font-size: %6px; }
        QPushButton#GridLineButton:hover { background: %7; }
        QPushButton#GridLineButton:checked { background: %8; color: white; }
        QPushButton#GridLineButton:disabled { color: rgba(42, 50, 67, 0.4); }
        QPushButton#GridLineButton:checked:disabled { background: %9; color: rgba(42, 50, 67, 0.4); }
        )")
        .arg(CONTROL_FILL.name(QColor::HexArgb)).arg(ink).arg(scaled(6)).arg(scaled(3)).arg(scaled(6)).arg(scaled(12))
        .arg(CONTROL_FILL_HOVER.name(QColor::HexArgb)).arg(accent).arg(groove);
    return QString(R"(
        QLabel { color: %1; background: transparent; font-size: %2px; }
        QSlider { background: transparent; min-height: %3px; }
        QSlider::groove:horizontal { height: %4px; background: %5; border-radius: %6px; }
        QSlider::sub-page:horizontal { background: %7; border-radius: %6px; }
        QSlider::add-page:horizontal { background: %5; border-radius: %6px; }
        QSlider::handle:horizontal {
            width: %8px; height: %8px; margin: %9px 0;
            background: white; border: 1px solid rgba(0, 0, 0, 0.25); border-radius: %10px;
        }
        QSlider::handle:horizontal:hover { border-color: %7; }
        QSlider::groove:horizontal:disabled, QSlider::sub-page:horizontal:disabled { background: %5; }
        QLabel:disabled { color: rgba(42, 50, 67, 0.4); }
        )")
        .arg(ink).arg(scaled(12)).arg(scaled(20)).arg(scaled(4)).arg(groove).arg(scaled(2)).arg(accent).arg(scaled(12)).arg(-scaled(4)).arg(scaled(6))
        + buttons;
}

QString WatermarkStylePopup::angle_field(const QString &layout) { return layout == LAYOUT_DIAMOND ? "diamond_angle" : "angle"; }

void WatermarkStylePopup::show_value(const QString &key, int value) { values_[key]->setText(QString::number(value) + value_suffix(key)); }

void WatermarkStylePopup::on_slider(const QString &key, int value) {
    show_value(key, value);
    for (const auto &row : ROWS)
        if (key == row.key) emit changed(key == "angle" ? angle_field(layout_) : key, row.to_style(value));
}

void WatermarkStylePopup::on_preset(const QString &color_hex) {
    color_button->set_color(QColor(color_hex));
    emit changed("color", QColor(color_hex).name());
}

void WatermarkStylePopup::load_style(const WatermarkStyle &style) {
    layout_ = style.layout;
    for (const auto &row : ROWS) {
        auto *slider = sliders_[row.key];
        slider->blockSignals(true);
        slider->setValue(QString(row.key) == "angle" ? (layout_ == LAYOUT_DIAMOND ? style.diamond_angle : style.angle) : row.from_style(style));
        slider->blockSignals(false);
        show_value(row.key, slider->value());
    }
    // The angle applies to the tile and diamond layouts, the grid lines to the diamond layout only.
    const bool rotates = style.layout == LAYOUT_TILE || style.layout == LAYOUT_DIAMOND;
    for (QWidget *widget : {static_cast<QWidget *>(sliders_["angle"]), static_cast<QWidget *>(labels_["angle"]), static_cast<QWidget *>(values_["angle"])})
        widget->setEnabled(rotates);
    sliders_["angle"]->setToolTip(rotates ? QString() : wm_tr("Angle applies to tiled and diamond watermarks"));
    const bool diamond = style.layout == LAYOUT_DIAMOND;
    QList<QWidget *> line_widgets{line_label_};
    for (auto *button : line_buttons_) line_widgets.append(button);
    for (auto *widget : line_widgets) {
        widget->setEnabled(diamond);
        widget->setToolTip(diamond ? QString() : wm_tr("Grid lines apply to diamond watermarks"));
    }
    for (auto it = line_buttons_.begin(); it != line_buttons_.end(); ++it) it.value()->setChecked(it.key() == style.line_style);
    color_button->set_color(QColor(style.color));
}

void WatermarkStylePopup::retranslate() {
    for (const auto &row : ROWS) labels_[row.key]->setText(wm_tr(row.text));
    line_label_->setText(wm_tr("Grid Lines"));
    for (auto it = line_buttons_.begin(); it != line_buttons_.end(); ++it) it.value()->setText(wm_tr(line_style_name(it.key())));
    color_button->setToolTip(wm_tr("Custom Color"));
}

WatermarkSettingsPanel::WatermarkSettingsPanel(QWidget *parent) : QWidget(parent) {
    setAttribute(Qt::WA_TranslucentBackground);
    auto *layout = new QHBoxLayout(this);
    text_field = new WatermarkTextField;
    connect(text_field, &WatermarkTextField::clicked, this, &WatermarkSettingsPanel::text_edit_requested);
    connect(text_field, &WatermarkTextField::cleared, this, &WatermarkSettingsPanel::text_cleared);
    layout->addWidget(text_field);
    layout->addWidget(make_separator());
    layout_combo = new PillComboBox(nullptr, Qt::AlignHCenter);
    for (const auto &key : watermark_layouts()) layout_combo->addItem(wm_tr(layout_name(key)), key);
    layout_combo->setToolTip(wm_tr("Position"));
    connect(layout_combo, &QComboBox::currentIndexChanged, this, [this](int) { on_layout_changed(); });
    layout->addWidget(layout_combo);
    style_button = new QPushButton;
    style_button->setIcon(ResourceManager::get_icon(ResourceManager::get_icon_path(QString::fromUtf8("颜色设置.svg"))));
    style_button->setToolTip(wm_tr("Watermark Style"));
    style_button->setFocusPolicy(Qt::NoFocus);
    style_button->setAttribute(Qt::WA_Hover, true);
    style_button->installEventFilter(this);
    layout->addWidget(style_button);
    font_combo = new PillComboBox;
    font_combo->setToolTip(wm_tr("Font"));
    font_combo->setMaxVisibleItems(12);
    fill_fonts();
    connect(font_combo, &QComboBox::currentIndexChanged, this, [this](int) { on_font_changed(); });
    layout->addWidget(font_combo);
    apply_scale();
}

void WatermarkSettingsPanel::paintEvent(QPaintEvent *) { paint_rounded_panel(this); }

void WatermarkSettingsPanel::apply_scale() {
    // The two combos have different fixed widths; a width cap in the stylesheet would override them.
    setStyleSheet(build_settings_panel_stylesheet(true, "1px", 0, 2000, true));
    const int mh = scaled(BASE_MARGIN_H), mv = scaled(BASE_MARGIN_V);
    layout()->setContentsMargins(mh, mv, mh, mv);
    layout()->setSpacing(scaled(BASE_SPACING));
    text_field->apply_scale();
    fit_layout_combo();
    font_combo->setFixedWidth(scaled(BASE_FONT_COMBO_WIDTH));
    const int side = scaled(BASE_BTN);
    style_button->setFixedSize(side, side);
    style_button->setIconSize(QSize(scaled(BASE_ICON), scaled(BASE_ICON)));
    style_button->setStyleSheet(QString("QPushButton { padding: 0px; border: none; border-radius: %1px; background: transparent; }"
                                        "QPushButton:hover { background: %2; }")
                                    .arg(scaled(6))
                                    .arg(CONTROL_FILL_HOVER.name(QColor::HexArgb)));
    if (style_popup_) style_popup_->apply_scale();
    adjustSize();
    update();
}

void WatermarkSettingsPanel::fit_layout_combo() {
    // The font size comes from the stylesheet, so it is applied before measuring.
    layout_combo->ensurePolished();
    const auto metrics = layout_combo->fontMetrics();
    int longest = 0;
    for (int i = 0; i < layout_combo->count(); ++i) longest = std::max(longest, metrics.horizontalAdvance(layout_combo->itemText(i)));
    const int chrome = scaled(PillComboBox::BASE_PADDING_LEFT + PillComboBox::BASE_ARROW_COLUMN + 6);
    layout_combo->setFixedWidth(std::max(scaled(BASE_LAYOUT_COMBO_MIN_WIDTH), longest + chrome));
}

void WatermarkSettingsPanel::fill_fonts() {
    font_combo->blockSignals(true);
    font_combo->clear();
    font_combo->addItem(wm_tr("Default Font"), QString());
    font_combo->setItemData(0, get_default_text_font_for_language(), Qt::ToolTipRole);
    for (const auto &family : get_available_text_fonts()) font_combo->addItem(family, family);
    font_combo->blockSignals(false);
}

bool WatermarkSettingsPanel::eventFilter(QObject *watched, QEvent *event) {
    if (watched == style_button) {
        if (event->type() == QEvent::Enter) show_style_popup();
        else if (event->type() == QEvent::Leave && style_popup_) style_popup_->close_soon();
    }
    return QWidget::eventFilter(watched, event);
}

void WatermarkSettingsPanel::show_style_popup() {
    if (!style_popup_) {
        style_popup_ = new WatermarkStylePopup(this);
        connect(style_popup_, &WatermarkStylePopup::changed, this, &WatermarkSettingsPanel::on_popup_changed);
    }
    style_popup_->load_style(style_);
    style_popup_->show_beside(this, style_button);
}

void WatermarkSettingsPanel::on_popup_changed(const QString &key, const QVariant &value) {
    style_ = style_.with_changes({{key, value}});
    emit style_changed(key, value);
}

void WatermarkSettingsPanel::on_layout_changed() {
    auto layout_key = layout_combo->currentData().toString();
    if (layout_key.isEmpty()) layout_key = LAYOUT_TILE;
    style_ = style_.with_changes({{"layout", layout_key}});
    if (style_popup_) style_popup_->load_style(style_);
    emit style_changed("layout", layout_key);
}

void WatermarkSettingsPanel::on_font_changed() {
    const auto family = font_combo->currentData().toString();
    style_ = style_.with_changes({{"font_family", family}});
    emit style_changed("font_family", family);
}

void WatermarkSettingsPanel::load_style(const WatermarkStyle &style) {
    style_ = style.normalized();
    const std::pair<PillComboBox *, QString> combos[] = {{layout_combo, style_.layout}, {font_combo, style_.font_family}};
    for (const auto &[combo, value] : combos) {
        const int index = combo->findData(value);
        combo->blockSignals(true);
        combo->setCurrentIndex(std::max(0, index));
        combo->blockSignals(false);
    }
    if (style_popup_) style_popup_->load_style(style_);
}

void WatermarkSettingsPanel::set_template(const QString &template_text, const QDateTime &when) { text_field->set_template(template_text, when); }

void WatermarkSettingsPanel::retranslate() {
    const auto &layouts = watermark_layouts();
    for (int index = 0; index < layouts.size(); ++index) layout_combo->setItemText(index, wm_tr(layout_name(layouts[index])));
    layout_combo->setToolTip(wm_tr("Position"));
    fit_layout_combo();
    style_button->setToolTip(wm_tr("Watermark Style"));
    font_combo->setToolTip(wm_tr("Font"));
    const auto current = font_combo->currentData().toString();
    fill_fonts();
    font_combo->blockSignals(true);
    font_combo->setCurrentIndex(std::max(0, font_combo->findData(current)));
    font_combo->blockSignals(false);
    text_field->retranslate();
    if (style_popup_) style_popup_->retranslate();
}
}
