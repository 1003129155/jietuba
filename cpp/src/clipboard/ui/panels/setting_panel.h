#pragma once
#include <QPoint>
#include <QString>
#include <QVariant>
#include <functional>
#include <utility>
#include <vector>

class QMenu;
class QWidget;

// The gear menu at the bottom right of the clipboard window.
namespace jietuba::clipboard {
// Multi-select paste separator presets: (menu text, separator).
const std::vector<std::pair<const char *, QString>> &MULTI_PASTE_SEPARATORS();
// A separator shown in a line edit: newline and tab written as \n and \t.
QString escape_separator(const QString &text);
QString unescape_separator(const QString &text);

// Current values and callbacks; the window owns the state, the menu only builds UI.
struct SettingMenuOptions {
    QString menu_style;
    std::function<QString(const char *)> tr;
    bool paste_with_html = true;
    bool auto_paste = true;
    bool close_after_paste = true;
    bool move_to_top = true;
    bool show_metadata = true;
    bool preserve_search = false;
    int window_opacity = 0;
    int current_font_size = 0;
    int current_display_lines = 1;
    QString current_image_size = "small";
    QString current_theme_name;
    QString current_group_bar_position = "right";
    QVariantList opacity_options;
    QVariantList font_size_options;
    std::function<void(bool)> on_toggle_paste_html;
    std::function<void(bool)> on_toggle_auto_paste;
    std::function<void(bool)> on_toggle_close_after_paste;
    std::function<void(bool)> on_toggle_move_to_top;
    std::function<void(bool)> on_toggle_show_metadata;
    std::function<void(bool)> on_toggle_preserve_search;
    std::function<void(int)> on_set_opacity;
    std::function<void(int)> on_set_font_size;
    std::function<void(int)> on_set_display_lines;
    std::function<void(const QString &)> on_set_image_size;
    std::function<void(const QString &)> on_set_theme;
    std::function<void()> on_add_item;
    // Without it the menu has no clear entry.
    std::function<void()> on_clear_history;
    std::function<void(const QString &)> on_set_group_bar_position;
    QString multi_paste_separator = "\n";
    QString multi_paste_order = "oldest_first";
    bool multi_paste_keep_merged = false;
    std::function<void(const QString &)> on_set_multi_paste_separator;
    std::function<void(const QString &)> on_set_multi_paste_order;
    std::function<void(bool)> on_toggle_multi_paste_keep_merged;
    // Global position the menu opens above.
    QPoint anchor_pos;
};

// Builds and pops up the menu without blocking, so the button can toggle it.
QMenu *show_setting_menu(QWidget *parent, const SettingMenuOptions &options);
}
