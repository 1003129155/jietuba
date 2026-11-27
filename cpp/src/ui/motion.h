#pragma once
#include <QEasingCurve>
#include <optional>

class QApplication;

namespace jietuba::motion {
// Every transition takes one of three lengths: a colour change; a small move or a small popup;
// a larger area.
constexpr int FAST_MS = 83, MID_MS = 167, SLOW_MS = 250;

// False while Windows' animation effects are switched off: transitions then land at once.
bool enabled();
// How long a transition of `ms` runs: `ms`, or 0 while animations are off.
int duration(int ms);
// For what enters or moves: quick at first, long to settle.
QEasingCurve decelerate();

// Menus and combo box lists stay out of sight until their first frame is painted, then fade in.
// Qt's own opening effects, which play in a stand-in window resized frame by frame, are turned off.
void install_popup_fade(QApplication &application);

// For tests: overrides the system setting; nullopt follows it again.
void set_enabled_override(std::optional<bool> on);
}
