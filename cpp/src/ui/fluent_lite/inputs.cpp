#include "ui/fluent_lite/inputs.h"
#include "core/ui_scale.h"
#include "ui/fluent_lite/text_context_menu.h"
#include "ui/fluent_lite/theme.h"
#include <QContextMenuEvent>
#include <QStyleOptionComboBox>
#include <QStylePainter>

namespace jietuba {
QString input_qss(const QWidget *widget) {
    const auto &t = ui_tokens(widget);
    auto s = [widget](double v) { return widget_scaled(widget, v); };
    return QString(R"(
    QComboBox, QLineEdit, QSpinBox, QDoubleSpinBox {
        min-height: %1px;
        padding: %2px %3px; color: %4;
        background: %5; border: 1px solid %6;
        border-radius: %7px;
        font: %8px %9; selection-background-color: %10;
    }
    QComboBox:hover, QLineEdit:hover, QSpinBox:hover, QDoubleSpinBox:hover {
        border-color: %11; background: %12;
    }
    QComboBox:focus, QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus {
        border: 1px solid %10; background: %12;
    }
    QComboBox:disabled, QLineEdit:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled {
        color: %13; background: %14; border-color: %6;
    }
    QComboBox::drop-down { width: %15px; border: none; }
    QComboBox QAbstractItemView {
        color: %4; background: %16; border: 1px solid %6;
        border-radius: %7px; padding: %17px; outline: none;
        selection-background-color: %18; selection-color: %4;
    }
    QComboBox QAbstractItemView::item { min-height: %1px; padding: %19px %20px; }
    QSpinBox::up-button, QDoubleSpinBox::up-button,
    QSpinBox::down-button, QDoubleSpinBox::down-button {
        width: %21px; border: none; background: transparent;
    }
    )")
        .arg(s(INPUT_CONTENT_HEIGHT)).arg(s(INPUT_PADDING_V)).arg(s(INPUT_PADDING_H))
        .arg(t.text, t.input_background, t.border).arg(s(INPUT_RADIUS)).arg(s(13))
        .arg(FONT_FAMILY, ACCENT, t.border_hover, t.surface_strong, t.text_disabled, t.surface_subtle)
        .arg(s(24)).arg(t.popup_background).arg(s(5)).arg(t.accent_soft).arg(s(3)).arg(s(8)).arg(s(18));
}

ComboBox::ComboBox(QWidget *parent) : QComboBox(parent) {
    _apply_theme();
    connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { _apply_theme(); });
}

void ComboBox::_apply_theme() {
    apply_style_sheet(this, input_qss(this));
    // A scaled floor must not exceed a width pinned with setFixedWidth: Qt would honour the minimum.
    setMinimumWidth(std::min(widget_scaled(this, BASE_MIN_WIDTH), maximumWidth()));
}

void ComboBox::paintEvent(QPaintEvent *) {
    QStylePainter painter(this);
    QStyleOptionComboBox option;
    initStyleOption(&option);
    const auto text = option.currentText;
    // Drawn without text, otherwise it would appear twice.
    option.currentText.clear();
    painter.drawComplexControl(QStyle::CC_ComboBox, option);
    if (text.isEmpty()) return;
    const auto &tokens = ui_tokens(this);
    painter.setPen(QColor::fromString(isEnabled() ? tokens.text : tokens.text_disabled));
    // The whole box rather than the style's text rect: this style draws no
    // arrow, so the text rect would push the text off centre.
    painter.drawText(rect(), Qt::AlignCenter, text);
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

DoubleSpinBox::DoubleSpinBox(QWidget *parent) : QDoubleSpinBox(parent) {
    _apply_theme();
    connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { _apply_theme(); });
    setAlignment(Qt::AlignCenter);
    install_text_context_menu(lineEdit());
}

void DoubleSpinBox::_apply_theme() { apply_style_sheet(this, input_qss(this)); }
}
