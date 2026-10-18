#pragma once
#include <utility>

// Size metrics of the clipboard manage window, following the window scale setting.
namespace jietuba::clipboard {
constexpr int MANAGE_DIALOG_BASE_WIDTH = 900;
constexpr int MANAGE_DIALOG_BASE_HEIGHT = 600;
// The default opening size, separate from the design base above, which only yields the chrome ratio below.
// At 1200 wide the four type cards' 12px descriptions just fit in one row.
constexpr int MANAGE_DIALOG_DEFAULT_WIDTH = 1200;
constexpr int MANAGE_DIALOG_DEFAULT_HEIGHT = 760;
// The lists and forms in all three columns scroll, so the minimum height only has to hold the title bar,
// the footer and each column's header; the minimum width has to hold the form's icon category tabs.
constexpr int MANAGE_DIALOG_MIN_WIDTH_BASE = 1000;
constexpr int MANAGE_DIALOG_MIN_HEIGHT_BASE = 420;

int manage_dialog_width();
int manage_dialog_height();
int manage_dialog_min_width();
int manage_dialog_min_height();
// The default size for a screen's available area (logical pixels) at a window scale: at 100% linear through
// 1920×1032→1200×760 and 2560×1392→1280×900, at least 1200 wide and capped at the latter; not limited to
// the area, the caller shrinks it.
std::pair<int, int> manage_dialog_size(int available_width, int available_height, double factor);
// The minimum size never exceeds the available area: what does not fit is left to the columns' scrolling.
std::pair<int, int> fit_manage_dialog_min_size(int available_width, int available_height);
// The default size for the area, within it and not below the minimum for that area.
std::pair<int, int> fit_manage_dialog_size(int available_width, int available_height);
// Chrome margins: the design's fixed ratio of real window to mockup, times the window scale.
int scale_x(int value);
int scale_y(int value);
// Font sizes, radii and padding: the window scale only, or text would be a size larger than other windows.
int scale_ui(int value);
}
