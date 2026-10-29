#pragma once
#include <QPoint>
#include <QString>
#include <QStringList>

class QMenu;

namespace jietuba {
class PinWindow;

// The pin's state the menu shows.
struct PinMenuState {
    bool toolbar_visible = false;
    bool stay_on_top = false;
    bool click_through = false;
    bool locked = false;
    bool border_enabled = true;
    bool text_selection_enabled = true;
    bool thumbnail_mode = false;
    bool text_mode = false;
    bool text_only = false;
    // Text with formatting to switch, and whether it shows.
    bool has_formatting = false;
    bool formatted = false;
    bool annotations_hidden = false;
    // Whether Esc can close the pin right now.
    bool close_shortcut_enabled = true;
    // What the pin shows when it is not a still image or text.
    enum class Content { Image, Animation, Files };
    Content content = Content::Image;
    bool animation_paused = false;
    double animation_speed = 1.0;
    // 1-based; frames is 0 until counted.
    int animation_frame = 0, animation_frames = 0;
    // The right-clicked file with those selected along with it; empty off the cards, where the menu acts on every file.
    QStringList chosen_files;
    // Whether files are selected, which the copy keys then take, and whether chosen_files are them.
    bool files_selected = false, chosen_selected = false;
};

// Only the currently effective keyboard binding; cleared and mouse bindings show nothing.
QString pin_shortcut_display(const QString &cfg_key);

// The pin window's context menu.
class PinContextMenu {
public:
    explicit PinContextMenu(PinWindow *parent) : parent(parent) {}
    void show(const QPoint &global_pos, const PinMenuState &state);
    PinWindow *parent;
private:
    static void setup_font(QMenu *menu);
    // Hover colour follows the theme.
    static QString menu_style();
    void add_menu_items(QMenu *menu, const PinMenuState &state);
};
}
