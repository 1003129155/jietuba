#include "ui/number_settings_panel.h"
#include "canvas/items/drawing_items.h"
#include "core/i18n.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include <QEvent>
#include <QPainter>
#include <QStyleOptionGraphicsItem>

namespace jietuba {
QPixmap render_number_style_preview(const QString &style, const QColor &color, int side, int number) {
    QPixmap pixmap(side, side);
    pixmap.fill(Qt::transparent);
    const double radius = side / 2.0 - 2.0;
    NumberItem item(number, QPointF(0, 0), radius, color, style);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.translate(side / 2.0, side / 2.0);
    const QStyleOptionGraphicsItem option;
    item.paint(&painter, &option, nullptr);
    painter.end();
    return pixmap;
}

NumberStylePopup::NumberStylePopup(QWidget *parent) : HoverPopup(parent) {
    // The strip centres on the preview, so the pointer usually brushes the middle
    // buttons first: hover is barely tinted and selected gets a clear theme frame
    // (not a solid fill, which would hide the dark glyph).
    const auto accent = accent_color();
    set_extra_stylesheet(QString("QToolButton { border: 1px solid transparent; border-radius: %1px; background: transparent; }"
                                 "QToolButton:hover { background-color: rgba(15, 23, 42, 0.06); }"
                                 "QToolButton:checked, QToolButton:checked:hover {"
                                 " background-color: rgba(%2, %3, %4, 0.15);"
                                 " border: 2px solid %5; }")
                             .arg(scaled(6)).arg(accent.red()).arg(accent.green()).arg(accent.blue()).arg(accent.name()));
    auto *layout = new QHBoxLayout(this);
    const int pad = scaled(4);
    layout->setContentsMargins(pad, pad, pad, pad);
    layout->setSpacing(scaled(2));
    const std::pair<QString, const char *> styles[] = {
        {NumberItem::STYLE_SOLID, "Solid"}, {NumberItem::STYLE_HOLLOW_BG, "Hollow Circle"},
        {NumberItem::STYLE_HOLLOW_ALL, "Outline Only"}, {NumberItem::STYLE_NO_CIRCLE, "Number Only"},
    };
    for (const auto &[style, tip] : styles) {
        auto *button = new QToolButton(this);
        button->setCheckable(true);
        button->setAutoRaise(true);
        button->setToolTip(i18n_tr(tip, "ArrowSettingsPanel"));
        button->setFocusPolicy(Qt::NoFocus);
        const QString key = style;
        connect(button, &QToolButton::clicked, this, [this, key] { choose(key); });
        layout->addWidget(button);
        buttons_[style] = button;
    }
    apply_scale();
}

void NumberStylePopup::apply_scale() {
    HoverPopup::apply_scale();
    if (buttons_.isEmpty()) return;
    const int side = scaled(BASE_ITEM_SIDE);
    const int cell = side + scaled(BASE_ITEM_PADDING);
    for (auto it = buttons_.begin(); it != buttons_.end(); ++it) {
        it.value()->setFixedSize(cell, cell);
        it.value()->setIconSize(QSize(side, side));
        it.value()->setIcon(render_number_style_preview(it.key(), PREVIEW_INK, side));
    }
    adjustSize();
}

void NumberStylePopup::set_current_style(const QString &requested) {
    const auto style = NumberItem::normalize_style(requested);
    for (auto it = buttons_.begin(); it != buttons_.end(); ++it) it.value()->setChecked(it.key() == style);
}

void NumberStylePopup::choose(const QString &style) {
    set_current_style(style);
    emit style_selected(style);
    hide();
}

NumberSettingsPanel::NumberSettingsPanel(QWidget *parent) : BaseSettingsPanel(parent) { init_ui(); }

void NumberSettingsPanel::build_extra_controls(QHBoxLayout *layout) {
    next_preview = new QLabel;
    next_preview->setAlignment(Qt::AlignCenter);
    // Drawn with the style so it shows what the next number will look like.
    set_own_style(next_preview, "background: transparent;");
    // Preview and chevrons share one pill like the other value controls.
    next_pill_ = new PillFrame;
    next_pill_layout_ = new QHBoxLayout(next_pill_);
    next_pill_layout_->setSpacing(0);
    next_pill_layout_->addWidget(next_preview);
    layout->insertWidget(0, next_pill_);
    next_value_ = 1;
    current_style_ = NumberItem::DEFAULT_STYLE;
    // Hovering the preview opens the style strip.
    next_preview->setAttribute(Qt::WA_Hover, true);
    next_preview->setToolTip(tr_panel("Number Style"));
    next_preview->installEventFilter(this);
    auto *btn_wrap = new QWidget;
    next_btn_layout_ = new QVBoxLayout(btn_wrap);
    next_btn_layout_->setContentsMargins(0, 0, 0, 0);
    next_btn_layout_->setSpacing(0);
    next_up_btn = new QToolButton;
    next_up_btn->setToolTip(tr_panel("Next Number"));
    next_btn_layout_->addWidget(next_up_btn);
    next_down_btn = new QToolButton;
    next_down_btn->setToolTip(tr_panel("Next Number"));
    next_btn_layout_->addWidget(next_down_btn);
    next_pill_layout_->addWidget(btn_wrap);
    connect(next_up_btn, &QToolButton::clicked, this, [this] { change_next_number(1); });
    connect(next_down_btn, &QToolButton::clicked, this, [this] { change_next_number(-1); });
}

void NumberSettingsPanel::apply_scale_sizes() {
    BaseSettingsPanel::apply_scale_sizes();
    const int preview = scaled(BASE_NEXT_PREVIEW);
    next_preview->setFixedSize(preview, preview);
    next_pill_->apply_scale();
    next_pill_layout_->setContentsMargins(scaled(4), 0, scaled(3), 0);
    const int half = scaled(PillFrame::BASE_HEIGHT) / 2;
    style_step_button(next_up_btn, "up");
    style_step_button(next_down_btn, "down");
    for (auto *button : {next_up_btn, next_down_btn}) button->setFixedSize(scaled(BASE_NEXT_BTN_WIDTH), half);
    if (style_popup_) style_popup_->apply_scale();
    // The preview is drawn at the label's pixel size, so it is redrawn after scaling.
    update_next_preview();
}

void NumberSettingsPanel::set_next_number(int value) {
    if (!next_preview) return;
    value = std::max(1, value);
    if (next_value_ == value) return;
    next_value_ = value;
    update_next_preview(value);
}

void NumberSettingsPanel::update_next_preview(std::optional<int> value) {
    if (!next_preview) return;
    next_preview->setPixmap(render_number_style_preview(current_style_, NumberStylePopup::PREVIEW_INK, next_preview->width(), value.value_or(next_value_)));
}

void NumberSettingsPanel::change_next_number(int delta) {
    const int value = std::max(1, next_value_ + delta);
    next_value_ = value;
    update_next_preview(value);
    emit next_number_changed(value);
}

bool NumberSettingsPanel::eventFilter(QObject *watched, QEvent *event) {
    if (watched == next_preview) {
        if (event->type() == QEvent::Enter) show_style_popup();
        else if (event->type() == QEvent::Leave && style_popup_) style_popup_->close_soon();
    }
    return BaseSettingsPanel::eventFilter(watched, event);
}

NumberStylePopup *NumberSettingsPanel::ensure_style_popup() {
    if (!style_popup_) {
        style_popup_ = new NumberStylePopup(this);
        connect(style_popup_, &NumberStylePopup::style_selected, this, [this](const QString &style) {
            set_style(style);
            emit style_changed(style);
        });
    }
    return style_popup_;
}

void NumberSettingsPanel::show_style_popup() {
    auto *popup = ensure_style_popup();
    popup->set_current_style(current_style_);
    popup->show_beside(this, next_preview);
}

void NumberSettingsPanel::set_style(const QString &style) {
    current_style_ = NumberItem::normalize_style(style);
    if (style_popup_) style_popup_->set_current_style(current_style_);
    update_next_preview();
}
}
