#pragma once
#include "clipboard/controllers/foreground_tracker.h"
#include <functional>

// Gives focus back to the target window, then presses Ctrl+V. Simulated keys have no
// recipient: they land wherever focus is at that moment, so activation comes first,
// retried on a timer because SetForegroundWindow returning does not mean it happened.
namespace jietuba::clipboard {
// Brings hwnd to the foreground, attaching to its input queue for the call.
bool set_foreground_window(Hwnd hwnd);
// Releases modifiers the user still holds from the hotkey that opened the window.
void release_modifiers();
bool send_ctrl_v();
// Activates hwnd and pastes; with no target (closed window) pastes into the current
// foreground unless that is this application.
void paste_to_target(Hwnd hwnd);
// Hands every paste_to_target call to handler instead of pressing keys; empty restores.
void set_paste_to_target_for_test(std::function<void(Hwnd)> handler);
}
