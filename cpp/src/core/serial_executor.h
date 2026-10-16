#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>

namespace jietuba {
// Runs tasks one at a time, in submission order, on one dedicated thread.
class SerialExecutor {
public:
    SerialExecutor() : worker_([this] { run(); }) {}
    ~SerialExecutor() { shutdown(false); }
    SerialExecutor(const SerialExecutor &) = delete;
    SerialExecutor &operator=(const SerialExecutor &) = delete;

    // Queues task; the future is invalid (valid() false) once shut down.
    template <class F>
    auto submit(F task) -> std::future<std::invoke_result_t<F>> {
        using Result = std::invoke_result_t<F>;
        auto packaged = std::make_shared<std::packaged_task<Result()>>(std::move(task));
        auto future = packaged->get_future();
        {
            std::lock_guard lock(mutex_);
            if (stopping_) return {};
            tasks_.push_back([packaged] { (*packaged)(); });
        }
        wake_.notify_one();
        return future;
    }
    bool on_worker_thread() const { return std::this_thread::get_id() == worker_id_; }
    // Stops after the running task; cancel_pending drops the queued ones, otherwise
    // they run first.
    void shutdown(bool cancel_pending) {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            if (cancel_pending) tasks_.clear();
        }
        wake_.notify_one();
        if (worker_.joinable() && !on_worker_thread()) worker_.join();
    }

private:
    void run() {
        worker_id_ = std::this_thread::get_id();
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
                if (tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            task();
        }
    }
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> tasks_;
    bool stopping_ = false;
    std::atomic<std::thread::id> worker_id_;
    std::thread worker_;
};
}
