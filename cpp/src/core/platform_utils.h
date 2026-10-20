#pragma once
#include <QString>
#include <QStringList>
#include <QtGlobal>
#include <functional>
#include <optional>
#include <utility>

namespace jietuba {
// Memory
void trim_working_set();
// While predicate returns true (e.g. a capture is in progress) a due trim is postponed.
void set_trim_busy_check(std::function<bool()> predicate);
// Debounced: only the last request within delay_ms runs.
void request_trim_working_set(int delay_ms = 1500);
// Replaces what a due trim runs; empty restores trim_working_set.
void set_trim_for_test(std::function<void()> trim);

// DPI: must run before QApplication is created.
void set_dpi_awareness();
// Effective DPI of the primary display. Qt's own scaling is disabled, so
// QScreen::logicalDotsPerInch() is always 96 and cannot be used for this.
double get_system_dpi(double fallback = 96.0);

// Taskbar grouping; must run before QApplication is created.
void set_app_user_model_id(const QString &app_id = "jietuba.app");

// Processes. PIDs are recycled, so (creation time, image name) identifies a process.
constexpr unsigned PROCESS_TERMINATE = 0x0001;
constexpr unsigned PROCESS_QUERY_LIMITED_INFORMATION = 0x1000;
std::optional<std::pair<quint64, QString>> get_process_identity(qint64 pid);
// Does not check identity; callers must confirm it with get_process_identity first.
bool terminate_process_by_pid(qint64 pid);

// Executables Windows' game bar has recognized as games, from HKCU\System\GameConfigStore. The key is
// undocumented: missing or changed, the list is empty.
QStringList recognized_game_executables();
// A game holds the foreground: a recognized game, windowed or not, or any program in exclusive full
// screen. Full-screen video and browsers report the same "busy" state as borderless games, hence the list.
bool is_game_in_foreground();

// Capture exclusion (Windows 10 2004+): the window stays visible but is absent from screenshots.
constexpr unsigned WDA_NONE = 0x00000000;
constexpr unsigned WDA_EXCLUDEFROMCAPTURE = 0x00000011;
bool set_window_exclude_from_capture(quintptr hwnd, bool exclude);

// Changes only the z-order band; toggling WindowStaysOnTopHint would make Qt
// recreate the native window and lose its state.
bool set_window_topmost(quintptr hwnd, bool topmost);

// Clicks and wheel pass through to the window below. Only for layered windows;
// Qt recomputes extended styles when window flags change, so call again afterwards.
bool set_window_click_through(quintptr hwnd, bool enabled);

// Windows 11 corner preference. Without rounding the system shadow goes too.
// DEFAULT does not restore rounding after DONOTROUND, so ROUND is set explicitly.
constexpr int DWMWA_WINDOW_CORNER_PREFERENCE = 33;
constexpr int DWMWCP_DONOTROUND = 1;
constexpr int DWMWCP_ROUND = 2;
bool supports_window_corner_preference();
bool set_window_rounded_corners(quintptr hwnd, bool rounded);

long long get_last_error();
// The HKCU Run entry starting this program at sign-in. Shared with the Python version, like
// the autostart_enabled setting: whichever version last turned it on starts at sign-in.
inline const QString AUTOSTART_APP_NAME = "Jietuba";
inline constexpr wchar_t AUTOSTART_REG_KEY[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run";
bool get_autostart();
// Writes or removes the Run entry; failures are logged.
void set_autostart(bool enabled);
}
