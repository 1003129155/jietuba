#include "ui/screenshot_countdown.h"
#include "core/platform_utils.h"
#include "core/ui_scale.h"
#include <QPainter>
#include <QScreen>

namespace jietuba {
ScreenshotCountdown::ScreenshotCountdown(QScreen *screen)
    : QLabel(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowTransparentForInput |
                       Qt::WindowDoesNotAcceptFocus | Qt::NoDropShadowWindowHint) {
    setObjectName("screenshotCountdown");
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAlignment(Qt::AlignCenter);
    setFixedSize(scaled(96), scaled(96));
    setStyleSheet(QString("QLabel { color: white; font-size: %1px; font-weight: bold; }").arg(scaled(56)));
    setText(QString::number(remaining_));
    if (screen) move(screen->geometry().center() - rect().center());
    timer_.setTimerType(Qt::PreciseTimer);
    timer_.setInterval(1000);
    connect(&timer_, &QTimer::timeout, this, [this] {
        if (--remaining_ > 0) {
            setText(QString::number(remaining_));
            return;
        }
        timer_.stop();
        hide();
        emit finished();
    });
    show();
    timer_.start();
}

// A stylesheet background is not painted on a translucent top-level label, so the disc is drawn here.
void ScreenshotCountdown::paintEvent(QPaintEvent *event) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(20, 24, 30, 210));
    painter.drawEllipse(rect());
    painter.end();
    QLabel::paintEvent(event);
}

// The system frame would outline the square window around the disc.
void ScreenshotCountdown::showEvent(QShowEvent *event) {
    QLabel::showEvent(event);
    disable_native_frame(static_cast<quintptr>(winId()));
}
}
