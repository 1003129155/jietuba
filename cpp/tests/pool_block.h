#pragma once
#include <QSemaphore>
#include <QThreadPool>
#include <memory>

// Holds every thread of the global pool while it lives, so work started meanwhile waits in the queue.
class PoolBlock {
public:
    PoolBlock() : state_(std::make_shared<State>()) {
        auto *pool = QThreadPool::globalInstance();
        count_ = pool->maxThreadCount();
        for (int i = 0; i < count_; ++i)
            pool->start([state = state_] {
                state->started.release();
                state->gate.acquire();
            });
        state_->started.acquire(count_);
    }
    ~PoolBlock() { state_->gate.release(count_); }
    PoolBlock(const PoolBlock &) = delete;
    PoolBlock &operator=(const PoolBlock &) = delete;

private:
    struct State {
        QSemaphore started;
        QSemaphore gate;
    };
    std::shared_ptr<State> state_;
    int count_ = 0;
};
