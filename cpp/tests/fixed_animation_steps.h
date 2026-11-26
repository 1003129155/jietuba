#pragma once
#include <QtCore/private/qabstractanimation_p.h>

// While alive, animations advance one fixed step per timer tick instead of by the clock: a test that
// samples the frames of an animation sees every step even when the machine is too busy to keep time.
class FixedAnimationSteps {
public:
    FixedAnimationSteps() { QUnifiedTimer::instance()->setConsistentTiming(true); }
    ~FixedAnimationSteps() { QUnifiedTimer::instance()->setConsistentTiming(false); }
    FixedAnimationSteps(const FixedAnimationSteps &) = delete;
    FixedAnimationSteps &operator=(const FixedAnimationSteps &) = delete;
};
