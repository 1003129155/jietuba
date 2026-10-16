#pragma once
#include <QMetaObject>
#include <QObject>
#include <QString>
#include <functional>
#include <optional>

namespace jietuba::clipboard {
// Window handles as integers; 0 is no window.
using Hwnd = quintptr;

// The current foreground window; 0 when unavailable.
Hwnd get_foreground_hwnd();
// Whether hwnd still names an existing, visible window.
bool is_alive(Hwnd hwnd);
std::optional<quint32> get_window_pid(Hwnd hwnd);
QString get_window_class(Hwnd hwnd);
// Windows that cannot take keyboard focus cannot receive the simulated keys.
bool can_take_focus(Hwnd hwnd);
quint32 get_current_pid();

// Remembers the last foreground window that can receive the paste. Ctrl+V has no
// recipient, so focus must go back to this window first. While the picker is shown
// every foreground switch is followed (the window may stay open between pastes);
// the picker itself, the taskbar, the desktop and no-activate windows never qualify,
// and an unqualified window keeps the previous target.
class ForegroundWindowTracker : public QObject {
    Q_OBJECT
public:
    // Marks the window that must not become the target (the picker window).
    void set_excluded(std::function<bool(Hwnd)> predicate) { is_excluded_ = std::move(predicate); }
    // The last target; 0 once that window is gone.
    Hwnd target_hwnd() const;
    // Samples the current foreground, then follows every later switch.
    void start();
    void stop();
    // Considers the current foreground once; true when the target changed.
    bool sample();

private:
    void on_foreground(Hwnd hwnd);
    bool consider(Hwnd hwnd);
    bool eligible(Hwnd hwnd) const;
    Hwnd target_hwnd_ = 0;
    std::function<bool(Hwnd)> is_excluded_;
    bool watching_ = false;
    QMetaObject::Connection connection_;
};
}
