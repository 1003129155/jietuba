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
namespace {
// An input's fill while it has the focus: opaque, a step off the control fill.
QString focused_input_fill(const UIThemeTokens &t) { return t.is_dark() ? "#1F1F1F" : "#FFFFFF"; }
}

QString input_qss(const QWidget *widget) {
    const auto &t = ui_tokens(widget);
    auto s = [widget](double v) { return widget_scaled(widget, v); };
    const QString focused_fill = focused_input_fill(t);
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

QString text_edit_qss(const QWidget *widget) {
    const auto &t = ui_tokens(widget);
    auto s = [widget](double v) { return widget_scaled(widget, v); };
    // The focus edge is one pixel thicker; the padding gives it back so the text does not move.
    return QString(R"(
    QTextEdit {
        color: %1; background: %2; border: 1px solid %3; border-bottom-color: %4; border-radius: %5px;
        padding: %6px %7px; font: %8px %9; selection-background-color: %10;
    }
    QTextEdit:hover { background: %11; }
    QTextEdit:focus { background: %12; border-bottom: %13px solid %10; padding-bottom: %14px; }
    QTextEdit:disabled { color: %15; background: %16; border-color: %3; }
    )")
        .arg(t.fg, t.control, t.control_border, t.control_border_bottom)
        .arg(s(INPUT_RADIUS))
        .arg(s(INPUT_PADDING_V))
        .arg(s(INPUT_PADDING_H))
        .arg(s(14))
        .arg(FONT_FAMILY, t.accent, t.control_hover, focused_input_fill(t))
        .arg(s(2))
        .arg(s(INPUT_PADDING_V - 1))
        .arg(t.fg_disabled, t.control_pressed);
}

namespace {
// What input_qss sets on an input painted by fluent_control_style(): font, palette, minimum
// height and hover tracking.
void apply_painted_input_look(QWidget *widget) {
    const auto &t = ui_tokens(widget);
    apply_sheet_palette(widget, css_color(t.fg), css_color(t.fg_disabled), css_color(t.control), css_color(t.control_pressed), css_color(t.accent));
    widget->setFont(fluent_control_font(widget, 14));
    widget->setMinimumHeight(widget_scaled(widget, INPUT_CONTENT_HEIGHT) + 2 * widget_scaled(widget, INPUT_PADDING_V) + 2);
    widget->updateGeometry();
    widget->update();
}

// Starts painting with fluent_control_style(); false when another sheet's rule reached the
// widget meanwhile and it went back to its sheet.
template <typename Input>
bool start_painting(Input *input, bool &painted) {
    if (painted) return true;
    painted = true;
    input->setAttribute(Qt::WA_Hover);
    input->setMouseTracking(true);
    input->setStyle(fluent_control_style());
    return painted;
}

void stop_painting(QWidget *widget) {
    widget->setStyle(nullptr);
    widget->setFont(QFont());
    widget->setPalette(QPalette());
}

// input_qss's colours: normal, hover, focus (line edits only), disabled; a later rule wins.
ControlBox input_box(const QWidget *widget, QStyle::State state, bool focus_rule) {
    const auto &t = ui_tokens(widget);
    if (!(state & QStyle::State_Enabled)) return {css_color(t.control_pressed), css_color(t.control_border), css_color(t.control_border)};
    if (focus_rule && (state & QStyle::State_HasFocus))
        return {css_color(focused_input_fill(t)), css_color(t.control_border), css_color(t.accent), widget_scaled(widget, 2)};
    const QColor fill = css_color(state & QStyle::State_MouseOver ? t.control_hover : t.control);
    return {fill, css_color(t.control_border), css_color(t.control_border_bottom)};
}
}

ComboBox::ComboBox(QWidget *parent) : QComboBox(parent) {
    _apply_theme();
    connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { _apply_theme(); });
}

void ComboBox::_apply_theme() {
    if (!wants_painting()) {
        if (!sheet_chosen_) use_style_sheet();
        else apply_style_sheet(this, input_qss(this));
    } else if (start_painting(this, painted_)) {
        apply_painted_input_look(this);
    }
    // A width pinned with setFixedWidth stays; otherwise a scaled floor, never above the maximum.
    if (minimumWidth() != maximumWidth()) setMinimumWidth(std::min(widget_scaled(this, BASE_MIN_WIDTH), maximumWidth()));
}

bool ComboBox::wants_painting() const {
    if (sheet_chosen_ || isEditable() || metaObject() != &ComboBox::staticMetaObject) return false;
    return !style_sheet_rule_reaches(this);
}

void ComboBox::use_style_sheet() {
    sheet_chosen_ = true;
    const bool had_sheet = !styleSheet().isEmpty();
    if (painted_) {
        painted_ = false;
        stop_painting(this);
    }
    if (!had_sheet) apply_style_sheet(this, input_qss(this));
}

ControlBox ComboBox::control_box(QStyle::State state) const { return input_box(this, state, false); }

void ComboBox::showPopup() {
    // The popup list is styled by input_qss; the sheet is taken only when it is first needed,
    // and its frame and metrics match the painted ones.
    if (painted_) use_style_sheet();
    QComboBox::showPopup();
}

bool ComboBox::event(QEvent *event) {
    const bool result = QComboBox::event(event);
    if (painted_ && (event->type() == QEvent::Polish || event->type() == QEvent::StyleChange) && !wants_painting()) use_style_sheet();
    return result;
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
    if (!themed_) return;
    if (!wants_painting()) {
        if (!sheet_chosen_) use_style_sheet();
        else apply_style_sheet(this, input_qss(this));
    } else if (start_painting(this, painted_)) {
        apply_painted_input_look(this);
    }
}

bool LineEdit::wants_painting() const {
    // Actions and the clear button are laid out inside the sheet's padding.
    if (sheet_chosen_ || !actions().isEmpty() || isClearButtonEnabled() || metaObject() != &LineEdit::staticMetaObject) return false;
    return !style_sheet_rule_reaches(this);
}

void LineEdit::use_style_sheet() {
    sheet_chosen_ = true;
    const bool had_sheet = !styleSheet().isEmpty();
    if (painted_) {
        painted_ = false;
        stop_painting(this);
    }
    if (!had_sheet) apply_style_sheet(this, input_qss(this));
}

ControlBox LineEdit::control_box(QStyle::State state) const { return input_box(this, state, true); }

bool LineEdit::event(QEvent *event) {
    const bool result = QLineEdit::event(event);
    const auto type = event->type();
    if (painted_ && (type == QEvent::Polish || type == QEvent::StyleChange || type == QEvent::ActionAdded) && !wants_painting()) use_style_sheet();
    // The clear button's action is named only after it is added, so it is looked for afterwards.
    if (painted_ && type == QEvent::ChildAdded)
        QMetaObject::invokeMethod(this, [this] {
            if (painted_ && !wants_painting()) use_style_sheet();
        }, Qt::QueuedConnection);
    return result;
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
    QSlider::sub-page:horizontal:disabled { background: %9; }
    QSlider::handle:horizontal:disabled { background: %9; border-color: transparent; }
    )").arg(s(4)).arg(t.control_border_bottom).arg(s(2)).arg(t.accent).arg(s(16)).arg(s(6)).arg(t.control_border_bottom).arg(s(8)).arg(t.fg_disabled));
    // Class-qualified like set_own_style, so tooltips do not inherit it; the value dims with the slider.
    apply_style_sheet(value_label_, QString(".QLabel { color: %1; font: %2px %3; background: transparent; } .QLabel:disabled { color: %4; }")
                                        .arg(t.fg_muted)
                                        .arg(s(13))
                                        .arg(FONT_FAMILY, t.fg_disabled));
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
