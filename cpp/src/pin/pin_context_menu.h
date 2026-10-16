#pragma once
#include <QPoint>
#include <QString>

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
    // Whether Esc can close the pin right now.
    bool close_shortcut_enabled = true;
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
