#include "clipboard/controllers/paste_keystroke.h"
#include "core/logger.h"
#include <QTimer>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace jietuba::clipboard {
namespace {
// About 200 ms in all. A window that still refuses activation (an elevated one, say)
// is not going to accept it; paste into the current focus instead of waiting longer.
constexpr int ACTIVATE_RETRY_MS = 20;
constexpr int ACTIVATE_MAX_ATTEMPTS = 10;
constexpr BYTE MODIFIER_KEYS[] = {VK_CONTROL, VK_SHIFT, VK_MENU, VK_LWIN, VK_RWIN};
std::function<void(Hwnd)> &test_handler() {
    static std::function<void(Hwnd)> handler;
    return handler;
}

bool foreground_is_own_process() {
    const Hwnd hwnd = get_foreground_hwnd();
    if (!hwnd) return false;
    return get_window_pid(hwnd) == get_current_pid();
}

void send_paste() {
    release_modifiers();
    send_ctrl_v();
}

void activate_then_paste(Hwnd hwnd, int attempt) {
    if (get_foreground_hwnd() == hwnd) {
        send_paste();
        return;
    }
    if (attempt >= ACTIVATE_MAX_ATTEMPTS) {
        // Not sent blindly while this process is still in front: this app's own shortcut handlers would take the keys.
        if (foreground_is_own_process()) {
            log_debug(T("目标窗口 {hwnd} 未切到前台，放弃本次补按键", {{"hwnd", static_cast<qulonglong>(hwnd)}}), "Clipboard");
            return;
        }
        log_debug(T("目标窗口 {hwnd} 未切到前台，按当前焦点粘贴", {{"hwnd", static_cast<qulonglong>(hwnd)}}), "Clipboard");
        send_paste();
        return;
    }
    set_foreground_window(hwnd);
    QTimer::singleShot(ACTIVATE_RETRY_MS, [hwnd, attempt] { activate_then_paste(hwnd, attempt + 1); });
}
}

bool set_foreground_window(Hwnd target) {
    if (!target) return false;
    const auto hwnd = reinterpret_cast<HWND>(target);
    // SetForegroundWindow does not restore a minimized window, so the keys would go nowhere.
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    const DWORD current_thread = GetCurrentThreadId();
    const DWORD target_thread = GetWindowThreadProcessId(hwnd, nullptr);
    // Attaching to the target's input queue lifts the foreground lock; the queues share
    // focus state while attached, so detach right after.
    const bool attached = target_thread && target_thread != current_thread && AttachThreadInput(current_thread, target_thread, TRUE);
    BringWindowToTop(hwnd);
    const bool result = SetForegroundWindow(hwnd);
    if (attached) AttachThreadInput(current_thread, target_thread, FALSE);
    return result;
}

void release_modifiers() {
    for (BYTE vk : MODIFIER_KEYS)
        if (GetAsyncKeyState(vk) & 0x8000) keybd_event(vk, 0, KEYEVENTF_KEYUP, 0);
}

bool send_ctrl_v() {
    keybd_event(VK_CONTROL, 0, 0, 0);
    keybd_event('V', 0, 0, 0);
    keybd_event('V', 0, KEYEVENTF_KEYUP, 0);
    keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0);
    return true;
}

void paste_to_target(Hwnd hwnd) {
    if (test_handler()) {
        test_handler()(hwnd);
        return;
    }
    if (!hwnd) {
        if (!foreground_is_own_process()) send_paste();
        return;
    }
    activate_then_paste(hwnd, 0);
}

void set_paste_to_target_for_test(std::function<void(Hwnd)> handler) { test_handler() = std::move(handler); }
}
