#include "ui/fluent_lite/navigation.h"
#include "core/ui_scale.h"
#include "ui/fluent_lite/cards.h"
#include <QPainter>

namespace jietuba {
namespace {
// Thin line between navigation groups, inset to match the selection pill.
class NavSeparator : public QWidget {
public:
    explicit NavSeparator(QWidget *parent) : QWidget(parent) { setFixedHeight(dialog_scaled(11)); }
protected:
    void paintEvent(QPaintEvent *) override {
        const int inset = dialog_scaled(13);
        QPainter painter(this);
        painter.fillRect(inset, height() / 2, width() - 2 * inset, 1, css_color(ui_tokens(this).separator));
    }
};
}

NavigationInterface::NavigationInterface(QWidget *parent) : QWidget(parent), group_(new QButtonGroup(this)) {
    group_->setExclusive(true);
    layout_ = new QVBoxLayout(this);
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->setSpacing(5);
    top_ = new QVBoxLayout;
    top_->setSpacing(5);
    bottom_ = new QVBoxLayout;
    bottom_->setSpacing(5);
    layout_->addLayout(top_);
    layout_->addStretch(1);
    layout_->addLayout(bottom_);
    set_own_style(this, "background: transparent;");
    connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { _apply_theme(); });
}

void NavigationInterface::addSeparator() { top_->addWidget(new NavSeparator(this)); }

QPushButton *NavigationInterface::addItem(const QString &route_key, const IconSource &icon, const QString &text, std::function<void()> on_click,
                                          NavigationItemPosition position, const QString &tooltip, const QString &tone) {
    auto *button = new QPushButton(text, this);
    button->setObjectName("FluentLiteNavItem");
    button->setCheckable(true);
    button->setIconSize(QSize(dialog_scaled(18), dialog_scaled(18)));
    button->setCursor(Qt::PointingHandCursor);
    // Labels are visible already; a tooltip only when it adds context.
    if (!tooltip.isEmpty()) button->setToolTip(tooltip);
    connect(button, &QPushButton::clicked, this, [this, route_key] { setCurrentItem(route_key); });
    if (on_click) connect(button, &QPushButton::clicked, this, [on_click] { on_click(); });
    group_->addButton(button);
    items_[route_key] = button;
    icons_[route_key] = icon;
    tones_[route_key] = tone;
    connect(button, &QPushButton::toggled, this, [this, route_key](bool) { apply_icon(route_key); });
    style_button(button);
    apply_icon(route_key);
    (position == NavigationItemPosition::BOTTOM ? bottom_ : top_)->addWidget(button);
    return button;
}

void NavigationInterface::style_button(QPushButton *button) {
    const auto &t = ui_tokens(button);
    apply_style_sheet(button, QString(R"(
            QPushButton#FluentLiteNavItem { min-height: %1px; padding: %2px %3px; text-align: left;
                color: %4; background: transparent; border: 1px solid transparent; border-radius: %5px;
                font: %6px %7; }
            QPushButton#FluentLiteNavItem:hover { color: %4; background: %8; }
            QPushButton#FluentLiteNavItem:checked { color: %9; background: %10;
                border: 1px solid transparent; border-left: 4px solid %11;
                padding-left: %12px; font-weight: 600; }
        )").arg(dialog_scaled(40)).arg(dialog_scaled(2)).arg(dialog_scaled(13)).arg(t.text).arg(dialog_scaled(11)).arg(dialog_scaled(14))
        .arg(FONT_FAMILY, t.surface_strong, t.accent_text, t.accent_soft, ACCENT).arg(dialog_scaled(10)));
}

void NavigationInterface::apply_icon(const QString &route_key) {
    auto *button = items_[route_key];
    const auto &icon = icons_[route_key];
    if (const auto *qicon = std::get_if<QIcon>(&icon)) {
        button->setIcon(*qicon);
        return;
    }
    const auto &tokens = ui_tokens(button);
    QString color;
    if (button->isChecked()) {
        color = tokens.accent_text;
    } else {
        const auto tone = tones_.value(route_key).isEmpty() ? QString("neutral") : tones_.value(route_key);
        color = tokens.is_dark() ? feature_tone(tone).dark_fg : feature_tone(tone).light_fg;
    }
    button->setIcon(tinted_icon(icon, color));
}

void NavigationInterface::_apply_theme() {
    for (auto it = items_.begin(); it != items_.end(); ++it) {
        style_button(it.value());
        apply_icon(it.key());
    }
}

void NavigationInterface::setCurrentItem(const QString &route_key) {
    auto *button = items_.value(route_key);
    if (!button) return;
    current_ = route_key;
    button->setChecked(true);
}

void NavigationInterface::clearCurrentItem() {
    group_->setExclusive(false);
    for (auto *button : items_) button->setChecked(false);
    group_->setExclusive(true);
    current_.clear();
}
}
