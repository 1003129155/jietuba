#include "pin/pin_toolbar.h"
#include "pin/pin_hover.h"
#include "pin/pin_window.h"
#include <QApplication>
#include <QPointer>
#include <QScreen>

namespace jietuba {
PinToolbar::PinToolbar(PinWindow *parent_pin_window, ToolSettingsManager *config_manager)
    : Toolbar(nullptr, toolbar_layout::PIN), parent_pin_window(parent_pin_window), config_manager(config_manager) {}

QWidget *PinToolbar::host_window() const { return parent_pin_window; }

void PinToolbar::reposition_self() {
    if (isVisible() && parent_pin_window) position_near_window(parent_pin_window);
}

void PinToolbar::position_near_window(PinWindow *pin_window) {
    if (manual_positioned_ || !pin_window) return;
    const auto pin_pos = pin_window->pos();
    const auto pin_size = pin_window->size();
    auto *screen = pin_window->screen();
    if (!screen) screen = QApplication::primaryScreen();
    const auto screen_rect = screen->geometry();
    const int spacing = 2;
    const int toolbar_width = width();
    const int toolbar_height = height();
    const int panel_extra = max_panel_height();
    int toolbar_x = pin_pos.x() + pin_size.width() - toolbar_width;
    const int below_y = pin_pos.y() + pin_size.height() + spacing;
    const int above_y = pin_pos.y() - toolbar_height - spacing;
    // The panel height counts below, like the capture toolbar.
    int toolbar_y;
    bool toolbar_below;
    if (below_y + toolbar_height + panel_extra <= screen_rect.bottom()) {
        toolbar_y = below_y;
        toolbar_below = true;
    } else {
        toolbar_y = above_y;
        toolbar_below = false;
    }
    toolbar_x = std::max(screen_rect.left(), std::min(toolbar_x, screen_rect.right() - toolbar_width));
    toolbar_y = std::max(screen_rect.top(), std::min(toolbar_y, screen_rect.bottom() - toolbar_height));
    toolbar_below_selection_ = toolbar_below;
    move(toolbar_x, toolbar_y);
}

void PinToolbar::setVisible(bool visible) {
    if (visible) {
        reload_layout();
        if (parent_pin_window) position_near_window(parent_pin_window);
    }
    Toolbar::setVisible(visible);
}

void PinToolbar::enterEvent(QEnterEvent *event) {
    Toolbar::enterEvent(event);
    set_part_hovered("toolbar", true);
}

void PinToolbar::leaveEvent(QEvent *event) {
    Toolbar::leaveEvent(event);
    set_part_hovered("toolbar", false);
}

void PinToolbar::show_more_popup() {
    const bool created = !more_popup();
    Toolbar::show_more_popup();
    if (created) connect(more_popup(), &ToolbarMorePopup::hover_changed, this, [this](bool hovered) { set_part_hovered("popup", hovered); });
}

void PinToolbar::hide_more_popup() {
    Toolbar::hide_more_popup();
    // Clicking a button in the popup hides it while the pointer is on it, and Leave may never arrive.
    set_part_hovered("popup", false);
}

void PinToolbar::open_layout_dialog() {
    // While the dialog is open the pointer is on neither the pin nor the toolbar; it counts as an open menu so
    // the toolbar does not hide meanwhile.
    const QPointer<PinWindow> pin = parent_pin_window;
    if (!pin || !pin->hover_controls) {
        Toolbar::open_layout_dialog();
        return;
    }
    pin->hover_controls->set_menu_open(true);
    Toolbar::open_layout_dialog();
    // The pin may have closed while the dialog was open.
    if (pin && pin->hover_controls) pin->hover_controls->set_menu_open(false);
}

void PinToolbar::set_part_hovered(const QString &part, bool hovered) {
    if (hovered) hovered_parts_.insert(part);
    else hovered_parts_.remove(part);
    report_hover(!hovered_parts_.isEmpty());
}

void PinToolbar::report_hover(bool hovered) {
    if (parent_pin_window && parent_pin_window->hover_controls) parent_pin_window->hover_controls->set_toolbar_hovered(hovered);
}

void PinToolbar::reset_auto_position() {
    set_manual_positioned(false);
    if (parent_pin_window) position_near_window(parent_pin_window);
}

void PinToolbar::sync_with_pin_window() {
    if (isVisible() && parent_pin_window) {
        position_near_window(parent_pin_window);
        sync_all_panels_position();
    }
}
}
