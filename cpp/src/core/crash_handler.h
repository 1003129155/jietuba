#pragma once
#include <QString>
#include <functional>

namespace jietuba {
// Writes to stderr, the crash log (independent of Logger) and the Logger if ready.
void write_crash(const QString &tag, const QString &msg);
// A worker thread's body: an exception escaping it is written to the crash log and ends
// only that thread, as on the reference's threads.
void run_thread_guarded(const QString &name, const std::function<void()> &body);
// Installs handlers for uncaught C++ exceptions and native crashes. Idempotent.
void install_crash_hooks();
}
