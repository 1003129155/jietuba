#pragma once
#include <QPushButton>
#include <functional>

namespace jietuba {
// The pin window's close and toolbar toggle buttons: creation, style and position.
class PinControlButtons {
public:
    static constexpr int BUTTON_SIZE = 24;
    static constexpr int MARGIN = 5;
    static constexpr int SPACING = 10;
    explicit PinControlButtons(QWidget *parent);
    void update_positions(int window_width);
    // Both buttons back on top.
    void raise_all();
    // PinHoverControls decides visibility; this only applies it.
    void set_visible(bool close, bool toolbar);
    void connect_signals(std::function<void()> close_handler, std::function<void()> toggle_toolbar_handler);
    QWidget *parent;
    QPushButton *close_button = nullptr;
    QPushButton *toolbar_toggle_button = nullptr;
private:
    QPushButton *create_button(const QString &icon_name, const char *tooltip, const QString &style);
};
}
