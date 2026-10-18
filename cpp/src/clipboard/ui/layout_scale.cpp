#include "clipboard/ui/layout_scale.h"
#include "core/ui_scale.h"
#include <algorithm>

namespace jietuba::clipboard {
namespace {
// The default size grows with the screen up to this size, reached at the large available area (at 100%).
constexpr int SMALL_AVAILABLE_WIDTH = 1920;
constexpr int SMALL_AVAILABLE_HEIGHT = 1032;
constexpr int LARGE_AVAILABLE_WIDTH = 2560;
constexpr int LARGE_AVAILABLE_HEIGHT = 1392;
constexpr int LARGE_WIDTH = 1280;
constexpr int LARGE_HEIGHT = 900;
// 900x600 is the mockup; the real window has 20/120 more chrome. Independent of the user's scale.
constexpr double CHROME_SCALE_X = (MANAGE_DIALOG_BASE_WIDTH + 20) / static_cast<double>(MANAGE_DIALOG_BASE_WIDTH);
constexpr double CHROME_SCALE_Y = (MANAGE_DIALOG_BASE_HEIGHT + 120) / static_cast<double>(MANAGE_DIALOG_BASE_HEIGHT);

// Read each time: the setting can change while the application runs.
double dialog_factor() { return get_dialog_scale().factor(); }

int scale_value(int value, double factor) {
    if (value == 0) return 0;
    return std::max<int>(1, static_cast<int>(python_round(value * factor)));
}
}

int manage_dialog_width() { return static_cast<int>(python_round(MANAGE_DIALOG_DEFAULT_WIDTH * dialog_factor())); }
int manage_dialog_height() { return static_cast<int>(python_round(MANAGE_DIALOG_DEFAULT_HEIGHT * dialog_factor())); }
int manage_dialog_min_width() { return static_cast<int>(python_round(MANAGE_DIALOG_MIN_WIDTH_BASE * dialog_factor())); }
int manage_dialog_min_height() { return static_cast<int>(python_round(MANAGE_DIALOG_MIN_HEIGHT_BASE * dialog_factor())); }

std::pair<int, int> manage_dialog_size(int available_width, int available_height, double factor) {
    const int small_w = MANAGE_DIALOG_DEFAULT_WIDTH;
    const int small_h = MANAGE_DIALOG_DEFAULT_HEIGHT;
    const int width = std::min(LARGE_WIDTH, std::max(small_w, static_cast<int>(small_w + (available_width / factor - SMALL_AVAILABLE_WIDTH) * (LARGE_WIDTH - small_w) / (LARGE_AVAILABLE_WIDTH - SMALL_AVAILABLE_WIDTH))));
    const int height = std::min(LARGE_HEIGHT, static_cast<int>(small_h + (available_height / factor - SMALL_AVAILABLE_HEIGHT) * (LARGE_HEIGHT - small_h) / (LARGE_AVAILABLE_HEIGHT - SMALL_AVAILABLE_HEIGHT)));
    return {static_cast<int>(python_round(width * factor)), static_cast<int>(python_round(height * factor))};
}

std::pair<int, int> fit_manage_dialog_min_size(int available_width, int available_height) {
    return {std::min(manage_dialog_min_width(), available_width), std::min(manage_dialog_min_height(), available_height)};
}

std::pair<int, int> fit_manage_dialog_size(int available_width, int available_height) {
    const auto [width, height] = manage_dialog_size(available_width, available_height, dialog_factor());
    const auto [min_width, min_height] = fit_manage_dialog_min_size(available_width, available_height);
    return {std::max(std::min(width, available_width), min_width), std::max(std::min(height, available_height), min_height)};
}

int scale_x(int value) { return scale_value(value, CHROME_SCALE_X * dialog_factor()); }
int scale_y(int value) { return scale_value(value, CHROME_SCALE_Y * dialog_factor()); }
int scale_ui(int value) { return scale_value(value, dialog_factor()); }
}
