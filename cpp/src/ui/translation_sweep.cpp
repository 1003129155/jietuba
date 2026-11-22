#include "ui/translation_sweep.h"
#include "core/platform_utils.h"
#include "ui/base_settings_panel.h"
#include <QLinearGradient>
#include <QPainter>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace jietuba {
namespace {
// The band leans: its middle moves this far left per pixel down.
constexpr double LEAN = 0.3;

// Slow at both ends of a crossing.
double eased(double t) { return 0.5 - 0.5 * std::cos(std::numbers::pi * std::clamp(t, 0.0, 1.0)); }
double band_width(const QRect &area) { return std::clamp(area.width() * 0.35, 120.0, 520.0); }
QColor with_alpha(QColor color, int alpha) {
    color.setAlpha(alpha);
    return color;
}

// The band's middle on the area's top row at ms, nullopt while it rests. It starts clear of the
// left edge and ends clear of the right one, its lean included.
std::optional<double> band_middle(const QRect &area, qint64 ms) {
    const qint64 phase = ms % TranslationSweep::PERIOD_MS;
    if (phase >= TranslationSweep::CROSS_MS) return std::nullopt;
    const double width = band_width(area);
    const double travel = area.width() + width + LEAN * area.height();
    return area.left() - width / 2 + travel * eased(double(phase) / TranslationSweep::CROSS_MS);
}
}

TranslationSweep::TranslationSweep(QWidget *parent) : QWidget(parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    hide();
    delay_.setSingleShot(true);
    connect(&delay_, &QTimer::timeout, this, &TranslationSweep::start);
    frames_.setInterval(FRAME_MS);
    connect(&frames_, &QTimer::timeout, this, &TranslationSweep::tick);
}

void TranslationSweep::begin() {
    if (waiting_) return;
    waiting_ = true;
    delay_.start(SHOW_DELAY_MS);
}

void TranslationSweep::end() {
    waiting_ = false;
    active_ = false;
    delay_.stop();
    frames_.stop();
    hide();
}

void TranslationSweep::start() {
    if (!waiting_) return;
    active_ = true;
    clock_.start();
    drawn_ms_ = 0;
    show();
    raise();
    frames_.start();
    update();
}

void TranslationSweep::tick() {
    const qint64 ms = now();
    const QRect area = this->area();
    update(QRegion(band_bounds(area, drawn_ms_)) + band_bounds(area, ms));
    drawn_ms_ = ms;
}

void TranslationSweep::hold_at_for_test(int ms) {
    held_ms_ = ms;
    update();
}

qint64 TranslationSweep::now() const { return held_ms_ ? *held_ms_ : clock_.elapsed(); }

QRect TranslationSweep::band_bounds(const QRect &area, qint64 ms) {
    const auto middle = band_middle(area, ms);
    if (!middle) return {};
    const double half = band_width(area) / 2 + 2;
    const double left = *middle - LEAN * area.height() - half, right = *middle + half;
    return QRect(QPoint(static_cast<int>(std::floor(left)), area.top()), QPoint(static_cast<int>(std::ceil(right)), area.bottom())).intersected(area);
}

void TranslationSweep::paint_band(QPainter &painter, const QRect &area, bool dark, qint64 ms) {
    const auto middle = band_middle(area, ms);
    if (!middle || area.isEmpty()) return;
    const double width = band_width(area);
    // Across the lines x + LEAN * y = constant, from one side of the band to the other.
    const QPointF from(*middle - width / 2, area.top());
    const double along = width / (1 + LEAN * LEAN);
    QLinearGradient fill(from, from + QPointF(along, along * LEAN));
    const QColor accent = accent_color();
    fill.setColorAt(0, with_alpha(accent, 0));
    if (dark) {
        fill.setColorAt(0.3, with_alpha(accent, 40));
        fill.setColorAt(0.5, QColor(255, 255, 255, 80));
        fill.setColorAt(0.7, with_alpha(accent, 40));
    } else {
        fill.setColorAt(0.5, with_alpha(accent, 90));
    }
    fill.setColorAt(1, with_alpha(accent, 0));
    painter.save();
    painter.setClipRect(area);
    painter.fillRect(band_bounds(area, ms), fill);
    painter.restore();
}

void TranslationSweep::paintEvent(QPaintEvent *) {
    if (!active_) return;
    QPainter painter(this);
    paint_band(painter, area(), dark(), now());
}

TranslationSweepWindow::TranslationSweepWindow(const QRect &screen_area, bool dark) : dark_(dark) {
    setWindowFlags(Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint | Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setGeometry(screen_area);
    set_window_exclude_from_capture(static_cast<quintptr>(winId()), true);
}
}
