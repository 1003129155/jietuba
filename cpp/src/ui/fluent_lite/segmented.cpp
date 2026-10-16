#include "ui/fluent_lite/segmented.h"
#include "core/ui_scale.h"
#include "ui/fluent_lite/theme.h"
#include <QFont>
#include <QFontMetrics>

namespace jietuba {
namespace {
int bold_text_width(const QString &text) {
    QFont font;
    font.setFamilies(FONT_FAMILIES());
    font.setPixelSize(dialog_scaled(12));
    font.setWeight(QFont::DemiBold);
    return QFontMetrics(font).horizontalAdvance(text);
}
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
    return QString(R"(
            QPushButton { padding: %1px %2px; color: %3; background: transparent;
                border: none; border-radius: %4px; font: %5px %6; }
            QPushButton:hover { color: %7; background: %8; }
            QPushButton:checked { color: %9; background: %10; font-weight: 600; }
        )").arg(dialog_scaled(4)).arg(dialog_scaled(14)).arg(t.text_muted).arg(dialog_scaled(9)).arg(dialog_scaled(12))
        .arg(FONT_FAMILY, t.text, t.surface_subtle, t.accent_text, t.accent_soft);
}

void SegmentedWidget::_apply_theme() {
    const auto &t = ui_tokens(this);
    set_own_style(this, QString("background: %1; border: 1px solid %2; border-radius: %3px;").arg(t.surface_subtle, t.border).arg(dialog_scaled(11)));
    for (auto *button : items_) apply_style_sheet(button, style());
}

void SegmentedWidget::addItem(const QString &route_key, const QString &text, std::function<void()> on_click) {
    insertItem(static_cast<int>(items_.size()), route_key, text, std::move(on_click));
}

void SegmentedWidget::insertItem(int index, const QString &route_key, const QString &text, std::function<void()> on_click) {
    auto *button = new QPushButton(text, this);
    button->setCheckable(true);
    apply_style_sheet(button, style());
    // The checked item is bold; measure it bold so the text is not clipped.
    button->setMinimumWidth(bold_text_width(text) + 2 * dialog_scaled(14) + 2);
    connect(button, &QPushButton::clicked, this, [this, route_key] { setCurrentItem(route_key); });
    if (on_click) connect(button, &QPushButton::clicked, this, [on_click] { on_click(); });
    group_->addButton(button);
    layout_->insertWidget(index, button);
    items_[route_key] = button;
    order_.insert(index, route_key);
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
