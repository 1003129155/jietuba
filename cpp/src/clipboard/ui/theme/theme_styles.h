#pragma once
#include "clipboard/ui/theme/themes.h"
#include <QString>

namespace jietuba::clipboard {
// The object name the bar styles below select: the group bar, the filter bar and the bottom bar.
inline const QString BAR_OBJECT_NAME = QStringLiteral("clipboardBar");

// Style sheets of the clipboard popup from a theme. Sizes follow the toolbar and panel
// scale (1px lines stay 1px); the manage window uses the window scale and its own styles.
class ThemeStyleGenerator {
public:
    explicit ThemeStyleGenerator(const Theme &theme) : theme_(theme) {}
    // opacity 0-100, 0 opaque.
    QString generate_window_style(int opacity = 0) const;
    QString generate_list_widget_style(int opacity = 0) const;
    QString generate_search_bar_style(int opacity = 0) const;
    QString generate_time_filter_bar_style(int opacity = 0) const;
    QString generate_quick_edit_style() const;
    QString generate_menu_style() const;
    // The rounded box around the search icon and field.
    QString generate_search_box_style(bool has_text = false) const;
    QString generate_clear_search_btn_style() const;
    QString generate_filter_toggle_btn_style() const;
    QString generate_time_filter_date_edit_style() const;
    QString generate_time_filter_calendar_style() const;
    QString generate_time_filter_action_btn_style(bool primary = false) const;
    QString generate_time_filter_close_btn_style() const;
    QString generate_time_filter_type_btn_style() const;
    QString generate_menu_btn_style() const;
    // The lines between the list and the bars around it.
    QString divider_color() const;
    // "#RRGGBB" or "#RGB" -> "r, g, b".
    static QString hex_to_rgb(const QString &hex_color);

private:
    // The background alpha as Python prints alpha / 255.
    static QString alpha_text(int opacity);
    Theme theme_;
};
}
