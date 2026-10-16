#pragma once
#include <QString>
#include <functional>

namespace jietuba {
// Tracks detached worker threads so an update can wait until none are running.
namespace background_tasks {
bool busy();
// Blocks new tasks if none are running; returns false while any task runs.
bool pause_if_idle();
void resume();
// Waits until no task runs; false when the time ran out first.
bool wait_idle(double timeout_seconds);
// Runs target on a detached thread. Throws std::runtime_error while paused.
void start_thread(std::function<void()> target, const QString &name = {});
}
}
