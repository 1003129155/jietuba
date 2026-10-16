#include "pin/pin_border_overlay.h"
#include <QPainter>

namespace jietuba {
PinBorderOverlay::PinBorderOverlay(QWidget *parent, double corner_radius, const QColor &border_color)
    : QWidget(parent), corner_radius(corner_radius), border_color(border_color) {
    // Transparent, mouse events pass through, never focused.
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_TranslucentBackground);
    setAutoFillBackground(false);
    setFocusPolicy(Qt::NoFocus);
}

void PinBorderOverlay::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QPen pen(border_color, 3);
    pen.setCosmetic(true);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    const auto r = QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5);
    painter.drawRoundedRect(r, corner_radius, corner_radius);
    painter.end();
}

void PinBorderOverlay::set_corner_radius(double radius) {
    corner_radius = radius;
    update();
}

void PinBorderOverlay::set_border_color(const QColor &color) {
    border_color = color;
    update();
}
}
