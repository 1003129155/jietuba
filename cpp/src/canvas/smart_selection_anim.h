#pragma once
#include <QEasingCurve>
#include <QElapsedTimer>
#include <QObject>
#include <QRectF>
#include <QTimer>
#include <functional>

namespace jietuba {
// Tweens the hover smart selection between windows and controls, where a
// direct jump reads as a flash. Each frame costs a near full-screen mask
// repaint, so the duration is kept short enough not to delay a quick click.
class SmartSelectionAnimator : public QObject {
public:
    static constexpr int DURATION_MS = 90;
    static constexpr int FRAME_MS = 16;
    // Below this summed corner travel the jump is applied directly: the tween
    // would be invisible. Neighbouring controls (e.g. 30 px buttons) still tween.
    static constexpr double MIN_TRAVEL_PX = 40;
    SmartSelectionAnimator(std::function<void(const QRectF &)> apply_rect, QObject *parent = nullptr);
    bool is_running() const { return timer_.isActive(); }
    // Current end point; empty when idle.
    QRectF target() const { return to_; }
    void animate_to(const QRectF &current, const QRectF &target);
    // Jumps immediately and stops any tween.
    void snap_to(const QRectF &target);
    void stop();
private:
    void tick();
    static QRectF lerp(const QRectF &a, const QRectF &b, double t);
    static double travel(const QRectF &a, const QRectF &b);
    std::function<void(const QRectF &)> apply_rect_;
    QRectF from_, to_;
    QElapsedTimer elapsed_;
    QEasingCurve curve_{QEasingCurve::OutCubic};
    QTimer timer_;
};
}
