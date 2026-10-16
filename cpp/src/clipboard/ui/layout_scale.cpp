#include "clipboard/ui/layout_scale.h"
#include "core/ui_scale.h"
#include <algorithm>

namespace jietuba::clipboard {
namespace {
// Share of the screen's available area the default size may take, so the buttons stay on small screens.
constexpr double SCREEN_FILL = 0.92;
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

std::pair<int, int> fit_manage_dialog_size(int available_width, int available_height) {
    const int width = std::min(manage_dialog_width(), static_cast<int>(available_width * SCREEN_FILL));
    const int height = std::min(manage_dialog_height(), static_cast<int>(available_height * SCREEN_FILL));
    return {std::max(width, manage_dialog_min_width()), std::max(height, manage_dialog_min_height())};
}

int scale_x(int value) { return scale_value(value, CHROME_SCALE_X * dialog_factor()); }
int scale_y(int value) { return scale_value(value, CHROME_SCALE_Y * dialog_factor()); }
int scale_ui(int value) { return scale_value(value, dialog_factor()); }
}
