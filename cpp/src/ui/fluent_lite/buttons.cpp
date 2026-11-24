#include "ui/fluent_lite/buttons.h"
#include "core/ui_scale.h"
#include <QDesktopServices>
#include <QUrl>

namespace jietuba {
namespace {
QString base_style(const QWidget *widget) {
    const auto &t = ui_tokens(widget);
    auto s = [widget](double v) { return widget_scaled(widget, v); };
    // min-height is the content height: 20 + 2 * 5 + the border makes the 32px control.
    return QString(R"(
    QPushButton { min-height: %1px; padding: %2px %3px; color: %4;
     background: %5; border: 1px solid %6; border-bottom-color: %7; border-radius: %8px;
     font: %9px %10; outline: none; }
    QPushButton:hover { background: %11; }
    QPushButton:pressed { background: %12; color: %13; }
    QPushButton:disabled { color: %14; background: %12; border-color: %6; }
    )").arg(s(INPUT_CONTENT_HEIGHT)).arg(s(INPUT_PADDING_V)).arg(s(16)).arg(t.fg, t.control, t.control_border, t.control_border_bottom)
        .arg(s(INPUT_RADIUS)).arg(s(14)).arg(FONT_FAMILY, t.control_hover, t.control_pressed, t.fg_muted, t.fg_disabled);
}

QString primary_style(const QWidget *widget) {
    auto s = [widget](double v) { return widget_scaled(widget, v); };
    return QString(R"(
    QPushButton { min-height: %1px; padding: %2px %3px; color: white;
     background: %4; border: 1px solid %4; border-bottom-color: %5; border-radius: %6px;
     font: 600 %7px %8; outline: none; }
    QPushButton:hover { background: %9; border-color: %9; border-bottom-color: %5; }
    QPushButton:pressed { background: %5; border-color: %5; color: rgba(255,255,255,.78); }
    QPushButton:disabled { color: rgba(255,255,255,.84); background: #687D8F; border-color: #687D8F; }
    )").arg(s(INPUT_CONTENT_HEIGHT)).arg(s(INPUT_PADDING_V)).arg(s(18)).arg(ACCENT, ACCENT_PRESSED).arg(s(INPUT_RADIUS)).arg(s(14)).arg(FONT_FAMILY, ACCENT_HOVER);
}

QString transparent_style(const QWidget *widget) {
    const auto &t = ui_tokens(widget);
    auto s = [widget](double v) { return widget_scaled(widget, v); };
    return QString(R"(
    QPushButton { min-height: %1px; padding: %2px %3px; color: %4;
     background: transparent; border: 1px solid transparent; border-radius: %5px;
     font: %6px %7; outline: none; }
    QPushButton:hover { color: %8; background: %9; }
    QPushButton:pressed { background: %10; color: %4; }
    QPushButton:disabled { color: %11; }
    )").arg(s(INPUT_CONTENT_HEIGHT)).arg(s(INPUT_PADDING_V)).arg(s(12)).arg(t.fg_muted).arg(s(INPUT_RADIUS)).arg(s(14)).arg(FONT_FAMILY, t.fg, t.item_hover, t.control_pressed, t.fg_disabled);
}
}

PushButton::PushButton(const QString &text, QWidget *parent, const IconSource &icon) : QPushButton(text, parent) {
    setCursor(cursor().shape());
    init_theme();
    if (!std::holds_alternative<std::monostate>(icon)) setIcon(icon);
}

void PushButton::init_theme() {
    // style_sheet() is virtual but not dispatched to subclasses during this
    // constructor; each subclass constructor applies its own style again.
    _apply_theme();
    connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { _apply_theme(); });
}

void PushButton::setIcon(const IconSource &icon) {
    theme_icon_source_ = icon;
    QPushButton::setIcon(to_qicon(icon, this));
}

void PushButton::setBaseIconSize(int base) {
    base_icon_size_ = base;
    _apply_theme();
}

QString PushButton::style_sheet() const { return base_style(this); }

void PushButton::_apply_theme() {
    apply_style_sheet(this, style_sheet());
    const int side = widget_scaled(this, base_icon_size_);
    setIconSize(QSize(side, side));
    if (!std::holds_alternative<std::monostate>(theme_icon_source_)) QPushButton::setIcon(to_qicon(theme_icon_source_, this));
}

ColorSwatchButton::ColorSwatchButton(QWidget *parent) : PushButton(QString(), parent) {
    setCursor(Qt::PointingHandCursor);
    _apply_theme();
}

void ColorSwatchButton::setFill(const QString &fill) {
    fill_ = fill;
    _apply_theme();
}

void ColorSwatchButton::setSelected(bool selected) {
    selected_ = selected;
    _apply_theme();
}

QString ColorSwatchButton::style_sheet() const {
    const auto &t = ui_tokens(this);
    auto s = [this](double v) { return widget_scaled(this, v); };
    const auto border = selected_ ? QString("2px solid %1").arg(ACCENT) : QString("1px solid %1").arg(t.control_border_bottom);
    const auto hover = selected_ ? border : QString("1px solid %1").arg(t.fg_muted);
    return QString(R"(
        QPushButton { background: %1; border: %2;
         border-radius: %3px; min-height: %4px;
         padding: %5px 0; color: %6;
         font: 600 %7px %8; outline: none; }
        QPushButton:hover { border: %9; }
        )").arg(fill_, border).arg(s(INPUT_RADIUS)).arg(s(INPUT_CONTENT_HEIGHT)).arg(s(INPUT_PADDING_V))
        .arg(t.fg).arg(s(14)).arg(FONT_FAMILY, hover);
}

PrimaryPushButton::PrimaryPushButton(const QString &text, QWidget *parent, const IconSource &icon) : PushButton(text, parent, icon) { _apply_theme(); }
QString PrimaryPushButton::style_sheet() const { return primary_style(this); }
TransparentPushButton::TransparentPushButton(const QString &text, QWidget *parent, const IconSource &icon) : PushButton(text, parent, icon) { _apply_theme(); }
QString TransparentPushButton::style_sheet() const { return transparent_style(this); }

HyperlinkButton::HyperlinkButton(const QString &url, const QString &text, QWidget *parent, const IconSource &icon)
    : TransparentPushButton(text, parent, icon), url_(url) {
    _apply_theme();
    connect(this, &QPushButton::clicked, this, [this] {
        if (!url_.isEmpty()) QDesktopServices::openUrl(QUrl(url_));
    });
}

QString HyperlinkButton::style_sheet() const { return transparent_style(this) + QString("QPushButton { color: %1; }").arg(ACCENT); }

TransparentToolButton::TransparentToolButton(const IconSource &icon, QWidget *parent) : QToolButton(parent) {
    _apply_theme();
    connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { _apply_theme(); });
    if (!std::holds_alternative<std::monostate>(icon)) setIcon(icon);
}

void TransparentToolButton::setBaseMetrics(int side, int icon_size) {
    base_side_ = side;
    base_icon_size_ = icon_size;
    _apply_theme();
}

void TransparentToolButton::setBaseIconSize(int base) {
    base_icon_size_ = base;
    _apply_theme();
}

void TransparentToolButton::setIcon(const IconSource &icon) {
    theme_icon_source_ = icon;
    QToolButton::setIcon(to_qicon(icon, this));
}

void TransparentToolButton::_apply_theme() {
    const auto &t = ui_tokens(this);
    auto s = [this](double v) { return widget_scaled(this, v); };
    setFixedSize(s(base_side_), s(base_side_));
    setIconSize(QSize(s(base_icon_size_), s(base_icon_size_)));
    apply_style_sheet(this, QString(R"(
            QToolButton { background: transparent; border: none; border-radius: %1px; }
            QToolButton:hover { background: %2; }
            QToolButton:pressed { background: %3; }
        )").arg(s(INPUT_RADIUS)).arg(t.item_hover, t.control_pressed));
    if (!std::holds_alternative<std::monostate>(theme_icon_source_)) QToolButton::setIcon(to_qicon(theme_icon_source_, this));
}
}
