#include "core/background_tasks.h"
#include "core/crash_handler.h"
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace jietuba::background_tasks {
namespace {
std::mutex lock;
std::condition_variable idle;
int active = 0;
bool paused = false;
}

bool busy() {
    std::lock_guard guard(lock);
    return active > 0;
}

bool pause_if_idle() {
    std::lock_guard guard(lock);
    if (active) return false;
    paused = true;
    return true;
}

void resume() {
    std::lock_guard guard(lock);
    paused = false;
}

bool wait_idle(double timeout_seconds) {
    std::unique_lock guard(lock);
    return idle.wait_for(guard, std::chrono::duration<double>(timeout_seconds), [] { return active == 0; });
}

void start_thread(std::function<void()> target, const QString &name) {
    {
        std::lock_guard guard(lock);
        if (paused) throw std::runtime_error("Application is preparing to update");
        ++active;
    }
    try {
        std::thread([target = std::move(target), name] {
#ifdef Q_OS_WIN
            if (!name.isEmpty()) SetThreadDescription(GetCurrentThread(), reinterpret_cast<PCWSTR>(name.utf16()));
#endif
            run_thread_guarded(name, target);
            std::lock_guard guard(lock);
            if (--active == 0) idle.notify_all();
        }).detach();
    } catch (...) {
        std::lock_guard guard(lock);
        if (--active == 0) idle.notify_all();
        throw;
    }
}
}
