#include "ui/fluent_lite/switch.h"
#include "core/ui_scale.h"
#include "ui/fluent_lite/theme.h"
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>

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
    setFixedSize(widget_scaled(this, 46), widget_scaled(this, 26));
    update();
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
    const QColor on = QColor::fromString(ACCENT);
    int r = static_cast<int>(off.red() + (on.red() - off.red()) * progress_);
    int g = static_cast<int>(off.green() + (on.green() - off.green()) * progress_);
    int b = static_cast<int>(off.blue() + (on.blue() - off.blue()) * progress_);
    if (!isEnabled()) r = 205, g = 211, b = 207;
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
}
