#include "ui/fluent_lite/titlebar.h"
#include "core/resource_manager.h"
#include "core/ui_scale.h"
#include "ui/fluent_lite/theme.h"
#include <QPainter>
#include <QPixmap>

namespace jietuba {
void FluentCaptionButton::setMaxState(bool is_maximized) {
    if (is_maximized_ == is_maximized) return;
    is_maximized_ = is_maximized;
    update();
}

void FluentCaptionButton::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const auto [color, background] = colors();
    painter.setPen(Qt::NoPen);
    painter.setBrush(background);
    painter.drawRect(rect());
    // Geometry from this button's width (46 at 100%), not fixed coordinates.
    const double factor = width() / 46.0;
    const double icon_side = 10.0 * factor;
    const auto center = QRectF(rect()).center();
    const double left = center.x() - icon_side / 2.0, top = center.y() - icon_side / 2.0;
    QPen pen(color, std::max(1.0, factor));
    pen.setCapStyle(Qt::SquareCap);
    pen.setJoinStyle(Qt::MiterJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    if (kind == MINIMIZE) {
        const double y = center.y() + 2.0 * factor;
        painter.drawLine(QPointF(left, y), QPointF(left + icon_side, y));
    } else if (kind == CLOSE) {
        painter.drawLine(QPointF(left, top), QPointF(left + icon_side, top + icon_side));
        painter.drawLine(QPointF(left + icon_side, top), QPointF(left, top + icon_side));
    } else if (!is_maximized_) {
        painter.drawRect(QRectF(left, top, icon_side, icon_side));
    } else {
        const double offset = 2.0 * factor;
        const double restore_side = icon_side - offset;
        painter.drawRect(QRectF(left, top + offset, restore_side, restore_side));
        painter.drawRect(QRectF(left + offset, top, restore_side, restore_side));
    }
}

TitleBarPinButton::TitleBarPinButton(QWidget *parent) : QAbstractButton(parent) {
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    icon_path = ResourceManager::get_icon_path(QString::fromUtf8("钉图.svg"));
    icon_ = ResourceManager::get_icon(icon_path);
    _apply_theme();
    connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { _apply_theme(); });
}

void TitleBarPinButton::_apply_theme() {
    setFixedSize(widget_scaled(this, 46), widget_scaled(this, 32));
    update();
}

std::pair<QColor, QColor> TitleBarPinButton::colors() const {
    const auto &tokens = ui_tokens(this);
    if (isChecked()) return {QColor::fromString(tokens.accent), QColor::fromString(tokens.accent_soft)};
    if (isDown()) return {QColor::fromString(tokens.text), QColor::fromString(tokens.accent_soft)};
    if (underMouse()) return {QColor::fromString(tokens.text), QColor::fromString(tokens.surface_hover)};
    return {QColor::fromString(tokens.text_muted), QColor(0, 0, 0, 0)};
}

void TitleBarPinButton::enterEvent(QEnterEvent *event) {
    update();
    QAbstractButton::enterEvent(event);
}

void TitleBarPinButton::leaveEvent(QEvent *event) {
    update();
    QAbstractButton::leaveEvent(event);
}

void TitleBarPinButton::paintEvent(QPaintEvent *) {
    const auto [color, background] = colors();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(background);
    painter.drawRect(rect());
    // Rendered at twice the size and scaled back; the rotated icon would look ragged otherwise.
    const int side = widget_scaled(this, 18);
    QPixmap pixmap = icon_.pixmap(side * 2, side * 2);
    if (pixmap.isNull()) return;
    QPainter tint(&pixmap);
    tint.setCompositionMode(QPainter::CompositionMode_SourceIn);
    tint.fillRect(pixmap.rect(), color);
    tint.end();
    pixmap.setDevicePixelRatio(2.0);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.translate(width() / 2.0, height() / 2.0);
    // Unpinned, the pin lies at an angle: the pale accent alone would not tell on from off.
    if (!isChecked()) painter.rotate(-45.0);
    painter.drawPixmap(QPointF(-side / 2.0, -side / 2.0), pixmap);
}

FluentTitleBar::FluentTitleBar(QWidget *parent) : TitleBar(parent) {
    setObjectName("fluentTitleBar");
    // The base buttons are replaced once here so no window draws its own caption buttons.
    for (auto *button : {minBtn, maxBtn, closeBtn}) {
        hBoxLayout->removeWidget(button);
        button->hide();
        button->deleteLater();
    }
    minBtn = new FluentCaptionButton(FluentCaptionButton::MINIMIZE, this);
    maxBtn = new FluentCaptionButton(FluentCaptionButton::MAXIMIZE, this);
    closeBtn = new FluentCaptionButton(FluentCaptionButton::CLOSE, this);
    connect(minBtn, &QAbstractButton::clicked, this, [this] { window()->showMinimized(); });
    connect(maxBtn, &QAbstractButton::clicked, this, [this] { toggle_max_state(window()); });
    connect(closeBtn, &QAbstractButton::clicked, this, [this] { window()->close(); });
    hBoxLayout->addWidget(minBtn);
    hBoxLayout->addWidget(maxBtn);
    hBoxLayout->addWidget(closeBtn);
    setFixedHeight(32);
    buttonLayout = hBoxLayout;
    iconLabel = new QLabel(this);
    iconLabel->setFixedSize(22, 22);
    iconLabel->setAlignment(Qt::AlignCenter);
    titleLabel = new QLabel(parent->windowTitle(), this);
    _apply_theme();
    connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { _apply_theme(); });
    hBoxLayout->insertWidget(0, iconLabel);
    hBoxLayout->insertWidget(1, titleLabel);
    connect(parent, &QWidget::windowTitleChanged, this, &FluentTitleBar::setTitle);
    connect(parent, &QWidget::windowIconChanged, this, &FluentTitleBar::setIcon);
    // Transparency scoped to the bar itself; an unqualified rule would cascade to children.
    apply_style_sheet(this, "QWidget#fluentTitleBar { background: transparent; }");
}

void FluentTitleBar::_apply_theme() {
    const auto &tokens = ui_tokens(this);
    setFixedHeight(widget_scaled(this, 32));
    iconLabel->setFixedSize(widget_scaled(this, 22), widget_scaled(this, 22));
    for (auto *button : {minBtn, maxBtn, closeBtn}) button->setFixedSize(widget_scaled(this, 46), widget_scaled(this, 32));
    set_own_style(titleLabel, QString("color: %1; font: %2px %3; background: transparent;").arg(tokens.text).arg(widget_scaled(this, 12)).arg(FONT_FAMILY));
    apply_caption_colors();
    place_centered_title();
}

void FluentTitleBar::apply_caption_colors() {
    const auto &tokens = ui_tokens(this);
    for (auto *button : {minBtn, maxBtn, closeBtn}) {
        button->setNormalColor(QColor::fromString(tokens.text_muted));
        button->setHoverColor(QColor::fromString(tokens.text));
        button->setPressedColor(QColor::fromString(tokens.text));
        button->setNormalBackgroundColor(QColor(0, 0, 0, 0));
        button->setHoverBackgroundColor(QColor::fromString(tokens.surface_hover));
        button->setPressedBackgroundColor(QColor::fromString(tokens.accent_soft));
    }
    closeBtn->setHoverColor(QColor("#FFFFFF"));
    closeBtn->setHoverBackgroundColor(QColor("#C42B1C"));
}

void FluentTitleBar::setTitle(const QString &title) {
    titleLabel->setText(title);
    place_centered_title();
}

void FluentTitleBar::setIcon(const QIcon &icon) {
    const int side = widget_scaled(this, 16);
    iconLabel->setPixmap(icon.pixmap(side, side));
    place_centered_title();
}

void FluentTitleBar::center_title() {
    hBoxLayout->removeWidget(iconLabel);
    hBoxLayout->removeWidget(titleLabel);
    title_centered_ = true;
    place_centered_title();
}

void FluentTitleBar::place_centered_title() {
    if (!title_centered_) return;
    titleLabel->adjustSize();
    const int gap = widget_scaled(this, 6);
    const auto pixmap = iconLabel->pixmap();
    const int icon_w = !pixmap.isNull() ? iconLabel->width() + gap : 0;
    const int left = (width() - icon_w - titleLabel->width()) / 2;
    if (icon_w) iconLabel->move(left, (height() - iconLabel->height()) / 2);
    titleLabel->move(left + icon_w, (height() - titleLabel->height()) / 2);
}

void FluentTitleBar::resizeEvent(QResizeEvent *event) {
    TitleBar::resizeEvent(event);
    place_centered_title();
}

void FluentTitleBar::update_max_state() {
    if (auto *button = qobject_cast<FluentCaptionButton *>(maxBtn)) button->setMaxState(window()->isMaximized());
}
}
