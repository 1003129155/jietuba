#pragma once
#include <QAnimationDriver>
#include <QCoreApplication>
#include <functional>

// While alive, animations move only when step() is called. Each step is followed by the events it queued,
// a paint among them, so a test that samples painted frames sees one after every step. Driven by a timer, a
// busy machine handles a run of overdue ticks before the paint they queued, and paints only the last.
class SteppedAnimationClock : public QAnimationDriver {
public:
    SteppedAnimationClock() { install(); }
    ~SteppedAnimationClock() override { uninstall(); }
    SteppedAnimationClock(const SteppedAnimationClock &) = delete;
    SteppedAnimationClock &operator=(const SteppedAnimationClock &) = delete;

    qint64 elapsed() const override { return isRunning() ? now_ : 0; }

    void step(int ms = 16) {
        // An animation just started joins the clock through queued calls.
        QCoreApplication::processEvents();
        if (isRunning()) {
            now_ += ms;
            advance();
        }
        QCoreApplication::processEvents();
    }

    // Steps until `done` holds, at most `limit` times; returns whether it holds.
    bool step_until(const std::function<bool()> &done, int limit = 400) {
        for (int i = 0; i < limit && !done(); ++i) step();
        return done();
    }

protected:
    void start() override {
        now_ = 0;
        QAnimationDriver::start();
    }

private:
    qint64 now_ = 0;
};
