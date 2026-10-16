#include "translation/ui/widgets.h"
#include "core/i18n.h"

namespace jietuba {
EllipsisAnimator::EllipsisAnimator(QObject *parent, std::function<void(const QString &)> on_tick) : QObject(parent), on_tick_(std::move(on_tick)), timer_(this) {
    timer_.setInterval(INTERVAL_MS);
    timer_.setTimerType(Qt::CoarseTimer);
    connect(&timer_, &QTimer::timeout, this, &EllipsisAnimator::advance);
}

void EllipsisAnimator::start() {
    step_ = 0;
    timer_.start();
}

void EllipsisAnimator::stop() {
    timer_.stop();
    step_ = 0;
}

QString EllipsisAnimator::text_for(int step) {
    auto base = i18n_tr("Translating...", "TranslationDialog");
    static const QString trailing = QString::fromUtf8(".。…");
    while (!base.isEmpty() && trailing.contains(base.back())) base.chop(1);
    return base + QString(step % STEPS, '.');
}

void EllipsisAnimator::advance() {
    step_ = (step_ + 1) % STEPS;
    if (on_tick_) on_tick_(text_for(step_));
}
}
