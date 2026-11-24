#include "ui/fluent_lite/inputs.h"
#include "core/ui_scale.h"
#include "ui/fluent_lite/text_context_menu.h"
#include "ui/fluent_lite/theme.h"
#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QPolygonF>
#include <QStyleOptionComboBox>
#include <QStylePainter>

namespace jietuba {
QString input_qss(const QWidget *widget) {
    const auto &t = ui_tokens(widget);
    auto s = [widget](double v) { return widget_scaled(widget, v); };
    const QString focused_fill = t.is_dark() ? "#1F1F1F" : "#FFFFFF";
    return QString(R"(
    QComboBox, QLineEdit, QSpinBox, QDoubleSpinBox {
        min-height: %1px;
        padding: %2px %3px; color: %4;
        background: %5; border: 1px solid %6; border-bottom-color: %7;
        border-radius: %8px;
        font: %9px %10; selection-background-color: %11;
    }
    QComboBox:hover, QLineEdit:hover, QSpinBox:hover, QDoubleSpinBox:hover {
        background: %12;
    }
    QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus {
        background: %13; border-bottom: %14px solid %11; padding-bottom: %15px;
    }
    QComboBox:focus { border-color: %6; border-bottom-color: %7; }
    QComboBox:disabled, QLineEdit:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled {
        color: %16; background: %17; border-color: %6;
    }
    QComboBox::drop-down { width: %18px; border: none; }
    QComboBox QAbstractItemView {
        color: %4; background: %19; border: 1px solid %6;
        border-radius: %20px; padding: %21px; outline: none;
        selection-background-color: %22; selection-color: %4;
    }
    QComboBox QAbstractItemView::item { min-height: %23px; padding: 0 %3px; border-radius: %8px; }
    QSpinBox::up-button, QDoubleSpinBox::up-button,
    QSpinBox::down-button, QDoubleSpinBox::down-button {
        width: %24px; border: none; background: transparent;
    }
    )")
        .arg(s(INPUT_CONTENT_HEIGHT)).arg(s(INPUT_PADDING_V)).arg(s(INPUT_PADDING_H))
        .arg(t.fg, t.control, t.control_border, t.control_border_bottom).arg(s(INPUT_RADIUS)).arg(s(14))
        .arg(FONT_FAMILY, t.accent, t.control_hover, focused_fill).arg(s(2)).arg(s(INPUT_PADDING_V - 1))
        .arg(t.fg_disabled, t.control_pressed)
        .arg(s(24)).arg(t.popup_background).arg(s(8)).arg(s(4)).arg(t.item_hover).arg(s(32)).arg(s(18));
}

ComboBox::ComboBox(QWidget *parent) : QComboBox(parent) {
    _apply_theme();
    connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { _apply_theme(); });
}

void ComboBox::_apply_theme() {
    apply_style_sheet(this, input_qss(this));
    // A width pinned with setFixedWidth stays; otherwise a scaled floor, never above the maximum.
    if (minimumWidth() != maximumWidth()) setMinimumWidth(std::min(widget_scaled(this, BASE_MIN_WIDTH), maximumWidth()));
}

QSize ComboBox::sizeHint() const {
    auto size = QComboBox::sizeHint();
    size.setHeight(widget_scaled(this, INPUT_CONTENT_HEIGHT + 2 * INPUT_PADDING_V + 2));
    return size;
}

QSize ComboBox::minimumSizeHint() const {
    auto size = QComboBox::minimumSizeHint();
    size.setHeight(widget_scaled(this, INPUT_CONTENT_HEIGHT + 2 * INPUT_PADDING_V + 2));
    return size;
}

void ComboBox::setChevronVisible(bool visible) {
    chevron_ = visible;
    update();
}

void ComboBox::paintEvent(QPaintEvent *) {
    QStylePainter painter(this);
    QStyleOptionComboBox option;
    initStyleOption(&option);
    const auto text = option.currentText;
    // Drawn without text, otherwise it would appear twice.
    option.currentText.clear();
    painter.drawComplexControl(QStyle::CC_ComboBox, option);
    const auto &tokens = ui_tokens(this);
    const auto color = css_color(isEnabled() ? tokens.fg : tokens.fg_disabled);
    if (!chevron_) {
        painter.setPen(color);
        painter.drawText(rect(), Qt::AlignCenter | Qt::TextSingleLine, text);
        return;
    }
    if (!text.isEmpty()) {
        const auto field = style()->subControlRect(QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxEditField, this);
        painter.setPen(color);
        painter.drawText(field, Qt::AlignVCenter | Qt::AlignLeft | Qt::TextSingleLine, fontMetrics().elidedText(text, Qt::ElideRight, field.width()));
    }
    // The style draws no arrow; the chevron sits in the drop-down column.
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(color, std::max(1, widget_scaled(this, 1.4)), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    const double cx = width() - widget_scaled(this, 18), cy = height() / 2.0;
    const double half = widget_scaled(this, 4), drop = widget_scaled(this, 2);
    painter.drawPolyline(QPolygonF{QPointF(cx - half, cy - drop), QPointF(cx, cy + drop), QPointF(cx + half, cy - drop)});
}

LineEdit::LineEdit(QWidget *parent, bool use_default_style) : QLineEdit(parent), themed_(use_default_style) {
    if (themed_) {
        _apply_theme();
        connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { _apply_theme(); });
    }
}

void LineEdit::_apply_theme() {
    if (themed_) apply_style_sheet(this, input_qss(this));
}

void LineEdit::contextMenuEvent(QContextMenuEvent *event) { show_text_context_menu(this, event->globalPos()); }

void TextEdit::contextMenuEvent(QContextMenuEvent *event) { show_text_context_menu(this, event->globalPos()); }

SpinBox::SpinBox(QWidget *parent) : QSpinBox(parent) {
    _apply_theme();
    connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { _apply_theme(); });
    // Centred like the combo boxes: the value is much narrower than the box.
    setAlignment(Qt::AlignCenter);
    install_text_context_menu(lineEdit());
}

void SpinBox::_apply_theme() { apply_style_sheet(this, input_qss(this)); }

ValueSlider::ValueSlider(QWidget *parent) : QWidget(parent), slider_(new QSlider(Qt::Horizontal, this)), value_label_(new QLabel(this)) {
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(slider_, 1);
    layout->addWidget(value_label_);
    value_label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    connect(slider_, &QSlider::valueChanged, this, [this](int value) {
        value_label_->setText(QString::number(value));
        emit valueChanged(value);
    });
    setRange(slider_->minimum(), slider_->maximum());
    _apply_theme();
    connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { _apply_theme(); });
}

void ValueSlider::setRange(int minimum, int maximum) {
    slider_->setRange(minimum, maximum);
    value_label_->setText(QString::number(slider_->value()));
}

void ValueSlider::_apply_theme() {
    const auto &t = ui_tokens(this);
    auto s = [this](double v) { return widget_scaled(this, v); };
    layout()->setSpacing(s(10));
    slider_->setMinimumWidth(s(96));
    slider_->setFixedHeight(s(INPUT_CONTENT_HEIGHT + 2 * INPUT_PADDING_V + 2));
    apply_style_sheet(slider_, QString(R"(
    QSlider { background: transparent; }
    QSlider::groove:horizontal { height: %1px; background: %2; border-radius: %3px; }
    QSlider::sub-page:horizontal { background: %4; border-radius: %3px; }
    QSlider::add-page:horizontal { background: %2; border-radius: %3px; }
    QSlider::handle:horizontal { width: %5px; height: %5px; margin: -%6px 0; background: white; border: 1px solid %7; border-radius: %8px; }
    QSlider::handle:horizontal:hover { border-color: %4; }
    )").arg(s(4)).arg(t.switch_off).arg(s(2)).arg(t.accent).arg(s(16)).arg(s(6)).arg(t.border_hover).arg(s(8)));
    set_own_style(value_label_, QString("color: %1; font: %2px %3; background: transparent;").arg(t.fg_muted).arg(s(13)).arg(FONT_FAMILY));
    // The width of the widest value in the font the style sheet sets, so the slider does not move while it is dragged.
    value_label_->ensurePolished();
    value_label_->setFixedWidth(QFontMetrics(value_label_->font()).horizontalAdvance(QStringLiteral("100")) + s(6));
}

DoubleSpinBox::DoubleSpinBox(QWidget *parent) : QDoubleSpinBox(parent) {
    _apply_theme();
    connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { _apply_theme(); });
    setAlignment(Qt::AlignCenter);
    install_text_context_menu(lineEdit());
}

void DoubleSpinBox::_apply_theme() { apply_style_sheet(this, input_qss(this)); }
}
