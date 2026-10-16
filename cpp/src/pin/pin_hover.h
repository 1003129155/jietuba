#pragma once
#include <QTimer>
#include <optional>

namespace jietuba {
class PinWindow;

// Visibility of the pin's hover controls (top-right buttons, toolbar), decided only in
// sync(). Hover, menus, editing, thumbnail mode and manual toggles update inputs and call sync().
// Toolbar rules, first match wins:
//   1. thumbnail mode or click-through -> hidden
//   2. toggled by hand -> as chosen
//   3. a drawing tool in use -> shown
//   4. "auto show toolbar" on and inside a hover session -> shown
//   5. otherwise -> hidden
// A hover session starts with the pointer on the pin, toolbar or context menu and ends
// a grace period after it leaves all of them, never while editing. In auto mode the end
// clears the manual choice; in manual mode the choice stays.
class PinHoverControls {
public:
    // Enough to move the pointer from the pin onto its toolbar.
    static constexpr int TOOLBAR_GRACE_MS = 2000;
    static constexpr int BUTTONS_GRACE_MS = 300;
    explicit PinHoverControls(PinWindow *win);
    void set_pin_hovered(bool hovered);
    // Leave also fires when moving onto the pin's own children, so the widget under the pointer decides.
    void recheck_pin_hovered();
    void set_toolbar_hovered(bool hovered);
    void set_menu_open(bool menu_open);
    void on_editing_changed();
    void toggle_toolbar();
    void stop();
    void sync();
private:
    bool toolbar_wanted() const;
    bool hover_active() const;
    void store_pin_hovered(bool hovered);
    bool mouse_over_pin() const;
    void on_hover_changed();
    void end_session();
    bool toolbar_visible() const;
    bool auto_toolbar() const;
    bool hover_buttons_enabled() const;
    PinWindow *win_;
    bool pin_hovered_ = false;
    bool toolbar_hovered_ = false;
    bool menu_open_ = false;
    bool in_session_ = false;
    // nullopt follows the rules; true / false is a manual show / hide.
    std::optional<bool> toolbar_choice_;
    QTimer *session_timer_;
    QTimer *buttons_timer_;
};
}
