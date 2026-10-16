#include "pin/pin_controls.h"
#include "core/i18n.h"
#include "core/resource_manager.h"

namespace jietuba {
namespace {
// Red glow.
const char *CLOSE_BUTTON_STYLE = R"(
            QPushButton {
                background-color: transparent;
                border: 2px solid transparent;
                border-radius: 14px;
                padding: 0px;
            }
            QPushButton:hover {
                background-color: rgba(231, 76, 60, 40);
                border: 2px solid rgba(231, 76, 60, 120);
            }
            QPushButton:pressed {
                background-color: rgba(231, 76, 60, 80);
                border: 2px solid rgba(231, 76, 60, 180);
            }
        )";
// Blue glow.
const char *TOOLBAR_BUTTON_STYLE = R"(
            QPushButton {
                background-color: transparent;
                border: 2px solid transparent;
                border-radius: 14px;
                padding: 0px;
            }
            QPushButton:hover {
                background-color: rgba(52, 152, 219, 40);
                border: 2px solid rgba(52, 152, 219, 120);
            }
            QPushButton:pressed {
                background-color: rgba(52, 152, 219, 80);
                border: 2px solid rgba(52, 152, 219, 180);
            }
        )";
}

PinControlButtons::PinControlButtons(QWidget *parent) : parent(parent) {
    close_button = create_button(QString::fromUtf8("关闭.svg"), "Close (ESC)", CLOSE_BUTTON_STYLE);
    toolbar_toggle_button = create_button(QString::fromUtf8("工具栏.svg"), "Show toolbar", TOOLBAR_BUTTON_STYLE);
}

QPushButton *PinControlButtons::create_button(const QString &icon_name, const char *tooltip, const QString &style) {
    auto *button = new QPushButton(parent);
    button->setFixedSize(BUTTON_SIZE, BUTTON_SIZE);
    button->setIcon(ResourceManager::get_icon(ResourceManager::get_resource_path("svg/" + icon_name)));
    button->setIconSize(QSize(BUTTON_SIZE, BUTTON_SIZE));
    button->setStyleSheet(style);
    // The pin window's translation context.
    button->setToolTip(i18n_tr(tooltip, "PinWindow"));
    button->hide();
    return button;
}

void PinControlButtons::update_positions(int window_width) {
    // Close at the top-right corner, the toolbar toggle to its left.
    const int close_x = window_width - BUTTON_SIZE - MARGIN;
    close_button->move(close_x, MARGIN);
    toolbar_toggle_button->move(close_x - BUTTON_SIZE - SPACING, MARGIN);
    raise_all();
}

void PinControlButtons::raise_all() {
    close_button->raise();
    toolbar_toggle_button->raise();
}

void PinControlButtons::set_visible(bool close, bool toolbar) {
    const std::pair<QPushButton *, bool> entries[] = {{close_button, close}, {toolbar_toggle_button, toolbar}};
    for (const auto &[button, visible] : entries) {
        if (visible != button->isHidden()) continue;
        button->setVisible(visible);
        if (visible) button->raise();
    }
}

void PinControlButtons::connect_signals(std::function<void()> close_handler, std::function<void()> toggle_toolbar_handler) {
    QObject::connect(close_button, &QPushButton::clicked, close_button, [close_handler] { close_handler(); });
    QObject::connect(toolbar_toggle_button, &QPushButton::clicked, toolbar_toggle_button, [toggle_toolbar_handler] { toggle_toolbar_handler(); });
}
}
