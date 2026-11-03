#include "ui/fluent_lite/switch.h"
#include "core/ui_scale.h"
#include "ui/fluent_lite/theme.h"
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>

namespace jietuba {
SwitchButton::SwitchButton(QWidget *parent) : QWidget(parent), animation_(new QPropertyAnimation(this, "progress", this)) {
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    animation_->setDuration(145);
    animation_->setEasingCurve(QEasingCurve::OutCubic);
    _apply_theme();
    connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { update(); });
}

void SwitchButton::_apply_theme() {
    setFixedSize(widget_scaled(this, compact_ ? 32 : 46), widget_scaled(this, compact_ ? 18 : 26));
    update();
}

void SwitchButton::setCompact(bool compact) {
    compact_ = compact;
    _apply_theme();
}

void SwitchButton::set_progress(double value) {
    progress_ = value;
    update();
}

void SwitchButton::setChecked(bool checked) {
    if (checked_ == checked) {
        progress_ = checked ? 1.0 : 0.0;
        update();
        return;
    }
    checked_ = checked;
    animate();
    emit checkedChanged(checked);
}

void SwitchButton::animate() {
    animation_->stop();
    animation_->setStartValue(progress_);
    animation_->setEndValue(checked_ ? 1.0 : 0.0);
    animation_->start();
}

void SwitchButton::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) {
        setChecked(!checked_);
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void SwitchButton::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Space || event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        setChecked(!checked_);
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void SwitchButton::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF track(0.5, 0.5, width() - 1, height() - 1);
    const QColor off = QColor::fromString(ui_tokens(this).switch_off);
    const QColor on = QColor::fromString(ui_tokens(this).accent);
    int r = static_cast<int>(off.red() + (on.red() - off.red()) * progress_);
    int g = static_cast<int>(off.green() + (on.green() - off.green()) * progress_);
    int b = static_cast<int>(off.blue() + (on.blue() - off.blue()) * progress_);
    painter.setOpacity(isEnabled() ? 1.0 : 0.45);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(r, g, b));
    painter.drawRoundedRect(track, track.height() / 2, track.height() / 2);
    // Measured from the real height, so the knob stays centred at any size.
    const double margin = height() * 3.0 / 26.0;
    const double knob = height() - margin * 2;
    const double x = margin + (width() - knob - margin * 2) * progress_;
    painter.setBrush(QColor("#FFFFFF"));
    painter.drawEllipse(QRectF(x, margin, knob, knob));
}

LabeledSwitch::LabeledSwitch(const QString &text, QWidget *parent) : QAbstractButton(parent), switch_(new SwitchButton(this)) {
    setText(text);
    setCheckable(true);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    switch_->setCompact(true);
    switch_->setFocusPolicy(Qt::NoFocus);
    switch_->setAttribute(Qt::WA_TransparentForMouseEvents);
    connect(this, &QAbstractButton::toggled, switch_, &SwitchButton::setChecked);
    connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { _apply_theme(); });
    _apply_theme();
}

QSize LabeledSwitch::sizeHint() const {
    return QSize(fontMetrics().horizontalAdvance(text()) + switch_->width() + widget_scaled(this, 16),
                 std::max(fontMetrics().height(), switch_->height()) + widget_scaled(this, 8));
}

void LabeledSwitch::_apply_theme() {
    auto label_font = font();
    label_font.setFamilies(FONT_FAMILIES());
    label_font.setPixelSize(widget_scaled(this, 12));
    setFont(label_font);
    switch_->setProperty("dialog_scale_factor", property("dialog_scale_factor"));
    switch_->_apply_theme();
    updateGeometry();
    update();
}

void LabeledSwitch::resizeEvent(QResizeEvent *event) {
    QAbstractButton::resizeEvent(event);
    const int inset = widget_scaled(this, 4);
    switch_->move(layoutDirection() == Qt::RightToLeft ? inset : width() - switch_->width() - inset, (height() - switch_->height()) / 2);
}

void LabeledSwitch::paintEvent(QPaintEvent *) {
    const auto &t = ui_tokens(this);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const double inset = widget_scaled(this, 1);
    const auto bounds = QRectF(rect()).adjusted(inset, inset, -inset, -inset);
    const int radius = widget_scaled(this, 4);
    if (hasFocus() || (isEnabled() && underMouse())) {
        painter.setPen(hasFocus() ? QPen(css_color(t.accent), 1) : QPen(Qt::NoPen));
        painter.setBrush(css_color(t.surface_subtle));
        painter.drawRoundedRect(bounds, radius, radius);
    }
    auto label = rect().adjusted(widget_scaled(this, 4), 0, -widget_scaled(this, 4), 0);
    const int gap = widget_scaled(this, 8);
    if (layoutDirection() == Qt::RightToLeft)
        label.setLeft(switch_->geometry().right() + gap);
    else
        label.setRight(switch_->geometry().left() - gap);
    painter.setPen(css_color(!isEnabled() ? t.text_disabled : underMouse() ? t.text : t.text_muted));
    painter.drawText(label, Qt::AlignVCenter | (layoutDirection() == Qt::RightToLeft ? Qt::AlignRight : Qt::AlignLeft), text());
}

void LabeledSwitch::enterEvent(QEnterEvent *event) {
    QAbstractButton::enterEvent(event);
    update();
}

void LabeledSwitch::leaveEvent(QEvent *event) {
    QAbstractButton::leaveEvent(event);
    update();
}
}
