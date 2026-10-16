#include "canvas/smart_selection_anim.h"
#include <algorithm>
#include <cmath>

namespace jietuba {
SmartSelectionAnimator::SmartSelectionAnimator(std::function<void(const QRectF &)> apply_rect, QObject *parent)
    : QObject(parent), apply_rect_(std::move(apply_rect)) {
    timer_.setTimerType(Qt::PreciseTimer);
    connect(&timer_, &QTimer::timeout, this, [this] { tick(); });
}

void SmartSelectionAnimator::animate_to(const QRectF &current, const QRectF &target) {
    if (target.isEmpty()) {
        stop();
        return;
    }
    // Hover sends the same target on every move; restarting would never finish.
    if (timer_.isActive() && target == to_) return;
    if (current.isEmpty() || travel(current, target) < MIN_TRAVEL_PX) {
        snap_to(target);
        return;
    }
    // A new target restarts from the current interpolated rect instead of queueing,
    // so the selection never chases the pointer after it stops.
    from_ = current;
    to_ = target;
    elapsed_.restart();
    if (!timer_.isActive()) timer_.start(FRAME_MS);
}

void SmartSelectionAnimator::snap_to(const QRectF &target) {
    stop();
    if (!target.isEmpty()) apply_rect_(target);
}

void SmartSelectionAnimator::stop() {
    timer_.stop();
    from_ = QRectF();
    to_ = QRectF();
}

void SmartSelectionAnimator::tick() {
    // Progress follows real time, so dropped frames shorten the frame count, not slow the motion.
    const double progress = std::min(1.0, elapsed_.elapsed() / static_cast<double>(DURATION_MS));
    const QRectF target(to_);
    if (progress >= 1.0) {
        stop();
        apply_rect_(target);
        return;
    }
    apply_rect_(lerp(from_, target, curve_.valueForProgress(progress)));
}

QRectF SmartSelectionAnimator::lerp(const QRectF &a, const QRectF &b, double t) {
    return {a.x() + (b.x() - a.x()) * t, a.y() + (b.y() - a.y()) * t,
            a.width() + (b.width() - a.width()) * t, a.height() + (b.height() - a.height()) * t};
}

double SmartSelectionAnimator::travel(const QRectF &a, const QRectF &b) {
    return std::abs(a.x() - b.x()) + std::abs(a.y() - b.y()) + std::abs(a.right() - b.right()) + std::abs(a.bottom() - b.bottom());
}
}
