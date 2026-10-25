#include "canvas/smart_selection_anim.h"
#include <algorithm>

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
    if (target == to_ && (timer_.isActive() || current == target)) return;
    if (current.isEmpty() || current == target) {
        snap_to(target);
        return;
    }
    to_ = target;
    from_ = current;
    if (!timer_.isActive()) {
        leg_start_ = 0;
        elapsed_.restart();
        timer_.start(FRAME_MS);
        return;
    }
    leg_start_ = progress();
    if (leg_start_ >= 1.0) snap_to(target);
}

void SmartSelectionAnimator::snap_to(const QRectF &target) {
    stop();
    if (!target.isEmpty()) apply_rect_(target);
}

void SmartSelectionAnimator::stop() {
    timer_.stop();
    from_ = QRectF();
    to_ = QRectF();
    leg_start_ = 0;
}

double SmartSelectionAnimator::progress() const {
    // Real time, so dropped frames shorten the frame count rather than slow the motion.
    return std::clamp(elapsed_.isValid() ? elapsed_.elapsed() / static_cast<double>(DURATION_MS) : 1.0, 0.0, 1.0);
}

void SmartSelectionAnimator::tick() {
    const double overall = progress();
    const QRectF from(from_), target(to_);
    const double leg = leg_start_ < 1.0 ? std::clamp((overall - leg_start_) / (1.0 - leg_start_), 0.0, 1.0) : 1.0;
    if (overall >= 1.0) stop();
    // Ease out (quadratic): fast start, gentle landing on the control.
    const double t = 1.0 - (1.0 - leg) * (1.0 - leg);
    apply_rect_(QRectF(from.topLeft() + (target.topLeft() - from.topLeft()) * t,
                       from.bottomRight() + (target.bottomRight() - from.bottomRight()) * t));
}
}
