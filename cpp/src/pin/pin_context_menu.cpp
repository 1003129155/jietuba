#include "pin/pin_context_menu.h"
#include "core/constants.h"
#include "core/i18n.h"
#include "core/shortcut_manager.h"
#include "core/theme.h"
#include "pin/pin_window.h"
#include "settings/tool_settings.h"
#include <QAction>
#include <QMenu>
#include <QPointer>

namespace jietuba {
namespace {
QString pin_tr(const char *text) { return i18n_tr(text, "PinWindow"); }
}

QString pin_shortcut_display(const QString &cfg_key) {
    const auto text = get_tool_settings_manager()->get_inapp_shortcut(cfg_key);
    // Defaults come from the settings reader; a binding the user cleared must not come back here.
    if (!parse_shortcut_to_qt(text) || is_reserved_inapp_shortcut(text)) return {};
    return inapp_shortcut_display_text(text);
}

void PinContextMenu::show(const QPoint &global_pos, const PinMenuState &state) {
    // Deleted after use, with its actions, so menus do not pile up on the pin. On the heap: if the pin is
    // deleted while the menu runs its own loop, it deletes the menu as a child, which a stack object cannot survive.
    const QPointer<QMenu> menu = new QMenu(parent);
    setup_font(menu);
    menu->setStyleSheet(menu_style());
    add_menu_items(menu, state);
    menu->exec(global_pos);
    delete menu.data();
}

void PinContextMenu::setup_font(QMenu *menu) {
    QFont menu_font("Microsoft YaHei UI", 9);
    if (!menu_font.exactMatch()) menu_font = QFont("Segoe UI", 9);
    menu->setFont(menu_font);
}

QString PinContextMenu::menu_style() {
    const auto tc = get_theme().theme_color_hex();
    return QString(R"(
            QMenu {
                background-color: white;
                border: 1px solid #ccc;
                border-radius: 4px;
                padding: 4px;
                font-family: %1, "Yu Gothic UI", sans-serif;
                font-size: 9pt;
                color: #000000;
            }
            QMenu::item {
                padding: 6px 12px;
                border-radius: 3px;
                color: #000000;
                background-color: transparent;
            }
            QMenu::item:selected {
                background-color: %2;
                color: #ffffff;
            }
            QMenu::item:disabled {
                color: #9e9e9e;
            }
            QMenu::separator {
                height: 1px;
                background: #ddd;
                margin: 4px 6px;
            }
        )").arg(CSS_FONT_FAMILY_UI, tc);
}

void PinContextMenu::add_menu_items(QMenu *menu, const PinMenuState &state) {
    const auto toggle_text = [](const QString &label, bool enabled) { return label + "\t" + (enabled ? QString::fromUtf8("●") : QString::fromUtf8("○")); };
    const auto shortcut_text = [](const QString &label, const QString &cfg_key) {
        const auto key = pin_shortcut_display(cfg_key);
        return key.isEmpty() ? label : QString("%1 (%2)").arg(label, key);
    };
    auto *win = parent;
    const auto add = [menu](QMenu *target, const QString &text, std::function<void()> slot) {
        auto *action = new QAction(text, menu);
        QObject::connect(action, &QAction::triggered, menu, [slot](bool) { slot(); });
        target->addAction(action);
    };
    const bool is_thumbnail = state.thumbnail_mode;
    add(menu, shortcut_text(pin_tr("Copy"), "inapp_copy_pin"), [win] { win->copy_to_clipboard(); });
    add(menu, pin_tr("Save as"), [win] { win->save_image(); });
    // Hidden in thumbnail mode.
    if (!is_thumbnail) {
        // Always available; without a result the OCR manager recognizes on demand.
        add(menu, pin_tr("Translate (Text Recognition)"), [win] { win->on_translate_clicked(); });
        add(menu, shortcut_text(pin_tr("Reset size"), "inapp_pin_reset_size"), [win] { win->reset_to_original_size(); });
        auto *transform_menu = new QMenu(pin_tr("Image transform"), menu);
        transform_menu->setStyleSheet(menu_style());
        setup_font(transform_menu);
        add(transform_menu, pin_tr("Rotate right"), [win] { win->rotate_image_cw(); });
        add(transform_menu, pin_tr("Rotate left"), [win] { win->rotate_image_ccw(); });
        add(transform_menu, pin_tr("Flip horizontal"), [win] { win->flip_image_horizontal(); });
        add(transform_menu, pin_tr("Flip vertical"), [win] { win->flip_image_vertical(); });
        transform_menu->addSeparator();
        add(transform_menu, pin_tr("Reset transform"), [win] { win->reset_image_transform(); });
        menu->addMenu(transform_menu);
    }
    menu->addSeparator();
    if (!is_thumbnail) add(menu, toggle_text(shortcut_text(pin_tr("Toolbar"), "inapp_toggle_toolbar"), state.toolbar_visible), [win] { win->toggle_toolbar(); });
    // Available in every mode.
    add(menu, toggle_text(pin_tr("Always on top"), state.stay_on_top), [win] { win->toggle_stay_on_top(); });
    add(menu, toggle_text(pin_tr("Mouse click-through"), state.click_through), [win] { win->toggle_click_through(); });
    add(menu, toggle_text(shortcut_text(pin_tr("Lock"), "inapp_pin_lock"), state.locked), [win] { win->toggle_lock(); });
    if (!is_thumbnail) {
        add(menu, toggle_text(pin_tr("Border"), state.border_enabled), [win] { win->toggle_border_effect(); });
        add(menu, toggle_text(pin_tr("Text selection"), state.text_selection_enabled), [win] { win->toggle_text_selection(); });
    }
    add(menu, toggle_text(shortcut_text(pin_tr("Thumbnail mode"), "inapp_thumbnail"), state.thumbnail_mode), [win] { win->toggle_thumbnail_mode(); });
    menu->addSeparator();
    auto close_label = pin_tr("Close");
    if (state.close_shortcut_enabled) close_label += " (ESC)";
    add(menu, close_label, [win] { win->close_window(); });
}
}
