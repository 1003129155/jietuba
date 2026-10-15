#pragma once
#include <utility>

// Size metrics of the clipboard manage window, following the window scale setting.
namespace jietuba::clipboard {
constexpr int MANAGE_DIALOG_BASE_WIDTH = 900;
constexpr int MANAGE_DIALOG_BASE_HEIGHT = 600;
// The default opening size, separate from the design base above, which only yields the chrome ratio below.
constexpr int MANAGE_DIALOG_DEFAULT_WIDTH = 1160;
constexpr int MANAGE_DIALOG_DEFAULT_HEIGHT = 760;
constexpr int MANAGE_DIALOG_MIN_WIDTH_BASE = 960;
constexpr int MANAGE_DIALOG_MIN_HEIGHT_BASE = 600;

int manage_dialog_width();
int manage_dialog_height();
int manage_dialog_min_width();
int manage_dialog_min_height();
// The default size shrunk to the screen's available area, never below the minimum.
std::pair<int, int> fit_manage_dialog_size(int available_width, int available_height);
// Chrome margins: the design's fixed ratio of real window to mockup, times the window scale.
int scale_x(int value);
int scale_y(int value);
// Font sizes, radii and padding: the window scale only, or text would be a size larger than other windows.
int scale_ui(int value);
}
