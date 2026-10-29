#include "pin/pin_hover.h"
#include "pin/pin_controls.h"
#include "pin/pin_toolbar.h"
#include "pin/pin_window.h"
#include "settings/tool_settings.h"
#include <QApplication>
#include <QCursor>

namespace jietuba {
PinHoverControls::PinHoverControls(PinWindow *win) : win_(win) {
    session_timer_ = new QTimer(win);
    session_timer_->setSingleShot(true);
    session_timer_->setInterval(TOOLBAR_GRACE_MS);
    QObject::connect(session_timer_, &QTimer::timeout, win, [this] { end_session(); });
    buttons_timer_ = new QTimer(win);
    buttons_timer_->setSingleShot(true);
    buttons_timer_->setInterval(BUTTONS_GRACE_MS);
    QObject::connect(buttons_timer_, &QTimer::timeout, win, [this] { sync(); });
}

void PinHoverControls::set_pin_hovered(bool hovered) {
    if (hovered == pin_hovered_) return;
    store_pin_hovered(hovered);
    on_hover_changed();
}

void PinHoverControls::recheck_pin_hovered() { set_pin_hovered(mouse_over_pin()); }

void PinHoverControls::set_toolbar_hovered(bool hovered) {
    if (hovered == toolbar_hovered_) return;
    toolbar_hovered_ = hovered;
    on_hover_changed();
}

void PinHoverControls::set_menu_open(bool menu_open) {
    menu_open_ = menu_open;
    if (win_->is_closed()) return;
    // The pin got Leave when the menu opened and may get no Enter after it closes.
    if (!menu_open) store_pin_hovered(mouse_over_pin());
    on_hover_changed();
}

void PinHoverControls::on_editing_changed() { on_hover_changed(); }

void PinHoverControls::toggle_toolbar() {
    if (win_->thumbnail_mode() || win_->click_through() || !win_->annotatable()) return;
    toolbar_choice_ = !toolbar_visible();
    in_session_ = true;
    if (!hover_active()) session_timer_->start();
    sync();
}

void PinHoverControls::stop() {
    session_timer_->stop();
    buttons_timer_->stop();
}

void PinHoverControls::sync() {
    auto *win = win_;
    if (win->is_closed()) return;
    const bool want_toolbar = toolbar_wanted();
    if (want_toolbar != toolbar_visible()) {
        if (want_toolbar) win->show_toolbar();
        else win->hide_toolbar();
    }
    const bool show_buttons = hover_buttons_enabled() && !win->click_through() && (pin_hovered_ || menu_open_ || buttons_timer_->isActive());
    win->control_buttons()->set_visible(show_buttons, show_buttons && !auto_toolbar() && !win->thumbnail_mode() && win->annotatable());
}

bool PinHoverControls::toolbar_wanted() const {
    if (win_->thumbnail_mode() || win_->click_through() || !win_->annotatable()) return false;
    if (toolbar_choice_) return *toolbar_choice_;
    if (win_->is_editing) return true;
    return auto_toolbar() && in_session_;
}

bool PinHoverControls::hover_active() const { return pin_hovered_ || toolbar_hovered_ || menu_open_; }

void PinHoverControls::store_pin_hovered(bool hovered) {
    pin_hovered_ = hovered;
    if (!hovered) buttons_timer_->start();
}

bool PinHoverControls::mouse_over_pin() const {
    auto *under = QApplication::widgetAt(QCursor::pos());
    return under && under->window() == win_;
}

void PinHoverControls::on_hover_changed() {
    if (hover_active()) {
        in_session_ = true;
        session_timer_->stop();
    } else if (in_session_ && !session_timer_->isActive()) {
        session_timer_->start();
    }
    sync();
}

void PinHoverControls::end_session() {
    // on_editing_changed restarts the timer once editing ends.
    if (hover_active() || win_->is_editing) return;
    in_session_ = false;
    if (auto_toolbar()) toolbar_choice_.reset();
    sync();
}

bool PinHoverControls::toolbar_visible() const { return win_->toolbar && win_->toolbar->isVisible(); }

bool PinHoverControls::auto_toolbar() const { return win_->config_manager ? win_->config_manager->get_pin_auto_toolbar() : true; }

bool PinHoverControls::hover_buttons_enabled() const {
    return win_->config_manager && win_->config_manager->get_pin_hover_buttons();
}
}
