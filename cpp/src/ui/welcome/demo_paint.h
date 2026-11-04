#pragma once
#include "ui/fluent_lite/icons.h"
#include "ui/welcome/base_page.h"
#include <QEasingCurve>
#include <QStringList>
#include <QTimer>
#include <QWidget>

class QPainter;

// Drawing shared by the wizard's illustrations: one small desktop, document window, cursor
// and key caps, so every page's demo shows the same scene.
namespace jietuba::welcome_demo {
QColor with_alpha(const QString &color, int alpha);
QColor with_alpha(QColor color, int alpha);
// 0 before start, 1 after end, linear between.
double phase(int now, int start, int end);
double ease(QEasingCurve::Type curve, double t);
QPointF lerp(const QPointF &from, const QPointF &to, double t);
// The window scale. Demos draw in 100% units under QPainter::scale(demo_scale()), so the whole
// picture grows with the window rather than each size on its own.
double demo_scale();
// Pixel-sized fonts fall back to system fonts, so CJK labels never show as boxes.
QFont pixel_font(int pixel_size, int weight = QFont::Normal);

// The desktop of a demo: a screen holding a document window with two text lines and a chart.
QRectF window_rect(const QRectF &screen);
QRectF chart_rect(const QRectF &screen);
// One of the chart's five bars.
QRectF chart_bar(const QRectF &chart, int index);
void draw_screen(QPainter &p, const WelcomeTheme &theme, const QRectF &screen);
void draw_chart(QPainter &p, const WelcomeTheme &theme, const QRectF &rect);
void draw_cursor(QPainter &p, const QPointF &tip);

// ── The program's own windows, small. ──
// The capture selection and pin colour (the theme colour setting).
QColor theme_color();
// An svg/ icon by file name, or a Fluent icon, tinted for the current theme; cached.
QIcon demo_icon(const QString &svg_name);
QIcon demo_icon(FluentIcon icon);
// The chart with a little room round it: what the demos select, pin and save.
QRectF chart_region(const QRectF &screen);
// A floating window's soft shadow.
void draw_shadow(QPainter &p, const WelcomeTheme &theme, const QRectF &box, double radius);
// The capture and screen annotation toolbars' buttons in their order; empty is a separator.
const QStringList &capture_tools();
const QStringList &annotation_tools();
// A toolbar as those two are: a rounded bar, the drag grip on its left, the checked button
// filled with the theme colour. toolbar_size gives its size at the origin.
QRectF toolbar_size(const QStringList &tools);
void draw_toolbar(QPainter &p, const WelcomeTheme &theme, const QRectF &bar, const QStringList &tools, int checked = -1);
// A screenshot t ms after it starts: the screen dims, a box is dragged round the chart, then
// the size label and the capture toolbar come up. checked lights a toolbar button.
void draw_capture(QPainter &p, const WelcomeTheme &theme, const QRectF &screen, int t, int checked = -1);
// The clipboard window: the history rows with kind icons and times, the group rail and the
// search bar. The first row slides in as fresh goes from 0 to 1.
QRectF clipboard_window_rect(const QRectF &screen);
void draw_clipboard_window(QPainter &p, const WelcomeTheme &theme, const QRectF &win, double fresh = 1);

// Key caps with a lower edge; a pressed cap is solid accent and sits on its edge.
double keycap_width(QPainter &p, const QString &label, double height);
void draw_keycap(QPainter &p, const WelcomeTheme &theme, const QRectF &box, const QString &label, bool pressed);

// A looping clock that runs only while the widget is shown.
class LoopingDemo : public QWidget {
public:
    explicit LoopingDemo(int duration_ms, QWidget *parent = nullptr);
    void set_time(int elapsed_ms);
    int time() const { return elapsed_ms_; }
protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void set_duration(int duration_ms);
    // After every time change, before the repaint.
    virtual void time_changed() {}
    // Fades in over the first fade_in ms of the loop and out over its last fade_out ms.
    double loop_opacity(int fade_in = 300, int fade_out = 500) const;
private:
    QTimer timer_;
    int duration_ms_;
    int elapsed_ms_ = 0;
};
}
