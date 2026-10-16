#pragma once
#include <QString>
#include <QtGlobal>
#include <array>
#include <optional>
#include <tuple>
#include <vector>

namespace jietuba {
using WindowRect = std::array<int, 4>;  // x1, y1, x2, y2 in physical pixels

struct WindowInfo {
    quintptr hwnd;
    WindowRect rect;
    QString title;
};

// Window rectangle without the Windows 10/11 drop shadow.
WindowRect get_window_rect_no_shadow(quintptr hwnd);
// Whether DWM hides the window (cloaked): windows on other virtual desktops and UWP apps the
// system suspended after closing still report visible, yet nothing of them is on screen.
bool is_window_cloaked(quintptr hwnd);

// Visible application windows in Z order (top first), for smart selection.
class WindowFinder {
public:
    WindowFinder(int screen_offset_x = 0, int screen_offset_y = 0);
    void set_screen_offset(int offset_x, int offset_y);
    // Enumerates visible, titled, non-tool, non-transparent windows of a sensible size.
    void find_windows();
    // Handles by exposed (unoccluded) area, largest first; fully covered windows are left out,
    // since hit testing takes the first window in Z order and they could never be picked.
    std::vector<quintptr> windows_by_exposed_area() const;
    // From the cached windows: once the overlay is up, a live hit test would find the overlay itself.
    std::optional<WindowInfo> find_window_at_point_info(int x, int y) const;
    WindowRect find_window_at_point(int x, int y, const std::optional<WindowRect> &fallback_rect = std::nullopt) const;
    // The monitor under the point; the outermost smart-selection level is that screen, not the whole desktop.
    WindowRect find_monitor_rect_at_point(int x, int y) const;
    void clear() { windows.clear(); }
    std::vector<WindowInfo> windows;
    int screen_offset_x, screen_offset_y;
    bool debug = false;
private:
    static WindowRect get_virtual_desktop_rect();
};

bool is_smart_selection_available();
std::optional<WindowRect> find_window_at_cursor(int screen_offset_x = 0, int screen_offset_y = 0);
}
