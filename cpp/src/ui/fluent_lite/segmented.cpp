#include "ui/fluent_lite/segmented.h"
#include "core/ui_scale.h"
#include "ui/fluent_lite/theme.h"
#include <QFont>
#include <QFontMetrics>

namespace jietuba {
namespace {
int bold_text_width(const QString &text, int pixel_size) {
    QFont font;
    font.setFamilies(FONT_FAMILIES());
    font.setPixelSize(dialog_scaled(pixel_size));
    font.setWeight(QFont::DemiBold);
    return QFontMetrics(font).horizontalAdvance(text);
}

struct Metrics {
    int font, pad_v, pad_h;
    int radius() const { return pad_v + 5; }
};
Metrics metrics(bool large) { return large ? Metrics{14, 7, 20} : Metrics{12, 4, 14}; }
}

SegmentedWidget::SegmentedWidget(QWidget *parent) : QWidget(parent), indicator_(ACCENT), group_(new QButtonGroup(this)), layout_(new QHBoxLayout(this)) {
    group_->setExclusive(true);
    layout_->setContentsMargins(dialog_scaled(3), dialog_scaled(3), dialog_scaled(3), dialog_scaled(3));
    layout_->setSpacing(dialog_scaled(3));
    _apply_theme();
    connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { _apply_theme(); });
}

QString SegmentedWidget::style() const {
    const auto &t = ui_tokens(this);
    const auto m = metrics(large_);
    return QString(R"(
            QPushButton { padding: %1px %2px; color: %3; background: transparent;
                border: none; border-radius: %4px; font: %5px %6; }
            QPushButton:hover { color: %7; background: %8; }
            QPushButton:checked { color: %9; background: %10; font-weight: 600; }
        )").arg(dialog_scaled(m.pad_v)).arg(dialog_scaled(m.pad_h)).arg(t.text_muted).arg(dialog_scaled(m.radius())).arg(dialog_scaled(m.font))
        .arg(FONT_FAMILY, t.text, t.surface_subtle, t.accent_text, t.accent_soft);
}

void SegmentedWidget::_apply_theme() {
    const auto &t = ui_tokens(this);
    set_own_style(this, QString("background: %1; border: 1px solid %2; border-radius: %3px;")
                            .arg(floating_ ? t.popup_background : t.surface_subtle, t.border)
                            .arg(dialog_scaled(metrics(large_).radius() + 2)));
    for (auto *button : items_) apply_style_sheet(button, style());
}

void SegmentedWidget::setFloating(bool floating) {
    if (floating_ == floating) return;
    floating_ = floating;
    // A QWidget subclass paints its style sheet background only with this attribute.
    setAttribute(Qt::WA_StyledBackground, floating);
    _apply_theme();
}

void SegmentedWidget::setLarge(bool large) {
    if (large_ == large) return;
    large_ = large;
    _apply_theme();
    refresh_items();
}

void SegmentedWidget::refresh_items() {
    const auto m = metrics(large_);
    for (auto *button : items_) button->setMinimumWidth(bold_text_width(button->text(), m.font) + 2 * dialog_scaled(m.pad_h) + 2);
}

void SegmentedWidget::addItem(const QString &route_key, const QString &text, std::function<void()> on_click) {
    insertItem(static_cast<int>(items_.size()), route_key, text, std::move(on_click));
}

void SegmentedWidget::insertItem(int index, const QString &route_key, const QString &text, std::function<void()> on_click) {
    auto *button = new QPushButton(text, this);
    button->setCheckable(true);
    apply_style_sheet(button, style());
    // The checked item is bold; measure it bold so the text is not clipped.
    const auto m = metrics(large_);
    button->setMinimumWidth(bold_text_width(text, m.font) + 2 * dialog_scaled(m.pad_h) + 2);
    connect(button, &QPushButton::clicked, this, [this, route_key] { setCurrentItem(route_key); });
    if (on_click) connect(button, &QPushButton::clicked, this, [on_click] { on_click(); });
    group_->addButton(button);
    layout_->insertWidget(index, button);
    items_[route_key] = button;
    order_.insert(index, route_key);
}

QString SegmentedWidget::itemText(const QString &route_key) const {
    auto *button = items_.value(route_key);
    return button ? button->text() : QString();
}

void SegmentedWidget::setCurrentItem(const QString &route_key) {
    if (!items_.contains(route_key)) return;
    items_[route_key]->setChecked(true);
    if (current_ != route_key) {
        current_ = route_key;
        emit currentItemChanged(route_key);
    }
}
}
