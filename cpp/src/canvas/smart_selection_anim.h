#pragma once
#include <QElapsedTimer>
#include <QObject>
#include <QRectF>
#include <QTimer>
#include <functional>

namespace jietuba {
// Tweens the hover smart selection between windows and controls, where a
// direct jump reads as a flash. A new target while a tween runs continues from
// the current rect but keeps the original deadline, so a quick sweep over many
// controls settles DURATION_MS after the first change instead of trailing the pointer.
class SmartSelectionAnimator : public QObject {
public:
    static constexpr int DURATION_MS = 120;
    static constexpr int FRAME_MS = 17;
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
    double progress() const;
    std::function<void(const QRectF &)> apply_rect_;
    QRectF from_, to_;
    // Overall progress at the last retarget; the remaining span maps to the new leg.
    double leg_start_ = 0;
    QElapsedTimer elapsed_;
    QTimer timer_;
};
}
