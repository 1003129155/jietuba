#pragma once
#include <QColor>
#include <QWidget>

namespace jietuba {
// A locked pin always gets this outline, border setting or not, to tell it apart at a glance.
inline const QColor LOCKED_BORDER_COLOR = QColor(255, 149, 0, 230);
// Pins without keyboard focus: the theme colour marks the one that takes keys.
inline const QColor UNFOCUSED_BORDER_COLOR = QColor(150, 150, 150, 200);

// One rounded theme-colour outline, no shadow: a single drawRoundedRect per frame.
class PinBorderOverlay : public QWidget {
public:
    explicit PinBorderOverlay(QWidget *parent = nullptr, double corner_radius = 2, const QColor &border_color = QColor(88, 94, 184, 200));
    void set_corner_radius(double radius);
    // Repaints only the ring under the stroke: the overlay is translucent, so a full update
    // would redraw the whole image beneath it on every focus change.
    void set_border_color(const QColor &color);
    double corner_radius;
    QColor border_color;
protected:
    void paintEvent(QPaintEvent *event) override;
};
}
