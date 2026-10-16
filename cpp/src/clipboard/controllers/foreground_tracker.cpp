#include "clipboard/controllers/foreground_tracker.h"
#include "core/input_hub.h"
#include "core/logger.h"
#include <QSet>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace jietuba::clipboard {
namespace {
// Taskbar, notification overflow, desktop: pasting there makes no sense.
const QSet<QString> &shell_classes() {
    static const QSet<QString> classes{"Shell_TrayWnd", "NotifyIconOverflowWindow", "TrayNotifyWnd", "Progman", "WorkerW"};
    return classes;
}

const QString WATCHER = "clipboard";

HWND as_hwnd(Hwnd hwnd) { return reinterpret_cast<HWND>(hwnd); }
}

Hwnd get_foreground_hwnd() { return reinterpret_cast<Hwnd>(GetForegroundWindow()); }

bool is_alive(Hwnd hwnd) { return hwnd && IsWindow(as_hwnd(hwnd)) && IsWindowVisible(as_hwnd(hwnd)); }

std::optional<quint32> get_window_pid(Hwnd hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(as_hwnd(hwnd), &pid);
    if (!pid) return std::nullopt;
    return pid;
}

QString get_window_class(Hwnd hwnd) {
    wchar_t buffer[256] = {};
    const int length = GetClassNameW(as_hwnd(hwnd), buffer, 256);
    if (length <= 0) return {};
    return QString::fromWCharArray(buffer, length);
}

bool can_take_focus(Hwnd hwnd) { return !(GetWindowLongW(as_hwnd(hwnd), GWL_EXSTYLE) & WS_EX_NOACTIVATE); }

quint32 get_current_pid() { return GetCurrentProcessId(); }

Hwnd ForegroundWindowTracker::target_hwnd() const { return is_alive(target_hwnd_) ? target_hwnd_ : 0; }

void ForegroundWindowTracker::start() {
    sample();
    if (watching_) return;
    auto *hub = input_hub();
    connection_ = connect(hub, &InputHub::foreground, this, [this](quintptr hwnd) { on_foreground(hwnd); }, Qt::QueuedConnection);
    hub->native->watch_foreground(WATCHER);
    watching_ = true;
}

void ForegroundWindowTracker::stop() {
    if (!watching_) return;
    watching_ = false;
    if (auto *hub = existing_input_hub()) hub->native->unwatch_foreground(WATCHER);
    disconnect(connection_);
}

bool ForegroundWindowTracker::sample() { return consider(get_foreground_hwnd()); }

void ForegroundWindowTracker::on_foreground(Hwnd hwnd) {
    // Switches delivered after stop no longer count.
    if (watching_) consider(hwnd);
}

bool ForegroundWindowTracker::consider(Hwnd hwnd) {
    if (!eligible(hwnd)) return false;
    if (hwnd == target_hwnd_) return false;
    target_hwnd_ = hwnd;
    log_debug(T("粘贴目标更新为 {hwnd} ({cls})", {{"hwnd", static_cast<qulonglong>(hwnd)}, {"cls", get_window_class(hwnd)}}), "Clipboard");
    return true;
}

bool ForegroundWindowTracker::eligible(Hwnd hwnd) const {
    if (!is_alive(hwnd)) return false;
    if (!can_take_focus(hwnd)) return false;
    if (shell_classes().contains(get_window_class(hwnd))) return false;
    return !is_excluded_ || !is_excluded_(hwnd);
}
}
