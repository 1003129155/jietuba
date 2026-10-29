#include "pin/pin_context_menu.h"
#include "core/constants.h"
#include "core/i18n.h"
#include "core/shortcut_manager.h"
#include "core/theme.h"
#include "pin/pin_animation.h"
#include "pin/pin_file_card.h"
#include "pin/pin_window.h"
#include "settings/tool_settings.h"
#include <QAction>
#include <QMenu>
#include <QPointer>
#include <cmath>
#include <functional>

namespace jietuba {
namespace {
QString pin_tr(const char *text) { return i18n_tr(text, "PinWindow"); }

// 1.0x, 1.5x, 0.25x.
QString speed_text(double speed) {
    const bool tenths = std::abs(speed * 10 - std::round(speed * 10)) < 1e-9;
    return QString::number(speed, 'f', tenths ? 1 : 2) + "x";
}
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
    const auto shortcut_text = [](const QString &label, const QString &cfg_key) {
        const auto key = pin_shortcut_display(cfg_key);
        return key.isEmpty() ? label : QString("%1 (%2)").arg(label, key);
    };
    auto *win = parent;
    const auto add = [menu](QMenu *target, const QString &text, std::function<void()> slot) {
        auto *action = new QAction(text, menu);
        QObject::connect(action, &QAction::triggered, menu, [slot](bool) { slot(); });
        target->addAction(action);
        return action;
    };
    // Switches show their state as a check mark, as in the tray menu.
    const auto add_switch = [&add](QMenu *target, const QString &text, bool on, std::function<void()> slot) {
        auto *action = add(target, text, std::move(slot));
        action->setCheckable(true);
        action->setChecked(on);
    };
    const bool is_thumbnail = state.thumbnail_mode;
    const bool is_animation = state.content == PinMenuState::Content::Animation, is_files = state.content == PinMenuState::Content::Files;
    if (is_files) {
        // The chosen files, or all of them; the copy keys act on the same files.
        const QStringList chosen = state.chosen_files;
        if (!chosen.isEmpty()) {
            const bool one = chosen.size() == 1;
            // A right-clicked file outside the selection is not what the copy keys take.
            const auto keyed = [&state, &shortcut_text](const QString &text, const char *key) { return state.chosen_selected ? shortcut_text(text, key) : text; };
            add(menu, pin_tr("Open"), [chosen, win] { PinFileCard::open(chosen, win); });
            add(menu, keyed(one ? pin_tr("Copy this file") : pin_tr("Copy selected files"), "inapp_copy_pin"), [chosen] { PinFileCard::copy_files(chosen); });
            add(menu, keyed(one ? pin_tr("Copy this file's path") : pin_tr("Copy selected file paths"), "inapp_copy_pin_text"),
                [chosen] { PinFileCard::copy_paths(chosen); });
            add(menu, pin_tr("Show in Explorer"), [chosen] { PinFileCard::reveal(chosen); });
        } else {
            const QStringList files = win->file_card()->files();
            add(menu, pin_tr("Copy all files"), [files] { PinFileCard::copy_files(files); });
            add(menu, shortcut_text(pin_tr("Copy all file paths"), "inapp_copy_pin_text"), [files] { PinFileCard::copy_paths(files); });
            add(menu, pin_tr("Show in Explorer"), [files] { PinFileCard::reveal(files); });
        }
        menu->addSeparator();
    }
    if (state.text_only) {
        // Copy takes the selected text, or all of it as shown.
        add(menu, shortcut_text(pin_tr("Copy text"), "inapp_copy_pin"), [win] { win->copy_to_clipboard(); });
        add(menu, pin_tr("Copy plain text"), [win] { win->copy_plain_text(); });
        add(menu, pin_tr("Copy image"), [win] { win->copy_image(); });
    } else if (is_animation) {
        add(menu, shortcut_text(pin_tr("Copy animation"), "inapp_copy_pin"), [win] { win->copy_animation(); });
        add(menu, pin_tr("Copy current frame"), [win] { win->copy_current_frame(); });
    } else if (is_files) {
        // With files chosen, the copy key takes them instead.
        const QString copy_image = pin_tr("Copy image");
        add(menu, state.files_selected ? copy_image : shortcut_text(copy_image, "inapp_copy_pin"), [win] { win->copy_image(); });
    } else {
        add(menu, shortcut_text(pin_tr("Copy"), "inapp_copy_pin"), [win] { win->copy_to_clipboard(); });
    }
    add(menu, pin_tr("Save as"), [win] { win->save_image(); });
    if (is_animation) {
        auto *animation = win->animation();
        menu->addSeparator();
        add(menu, state.animation_paused ? pin_tr("Play") : pin_tr("Pause"), [animation] { animation->toggle_paused(); });
        add(menu, pin_tr("Next frame"), [animation] { animation->step(1); });
        add(menu, pin_tr("Previous frame"), [animation] { animation->step(-1); });
        menu->addSeparator();
        add(menu, pin_tr("Faster"), [animation] { animation->change_speed(1); });
        add(menu, pin_tr("Slower"), [animation] { animation->change_speed(-1); });
        add(menu, pin_tr("Normal speed"), [animation] { animation->change_speed(0); });
        add(menu, pin_tr("Speed: %1").arg(speed_text(state.animation_speed)), [] {})->setEnabled(false);
        const auto frame = state.animation_frames > 0 ? QString("%1/%2").arg(state.animation_frame).arg(state.animation_frames) : QString::number(state.animation_frame);
        add(menu, pin_tr("Frame: %1").arg(frame), [] {})->setEnabled(false);
    }
    // Always available; without a result the OCR manager recognizes on demand. A text pin
    // translates its own text. An animation or a file list has no text to read.
    if (!is_thumbnail && !is_animation && !is_files) {
        add(menu, state.text_only ? pin_tr("Translate") : pin_tr("Translate (Text Recognition)"), [win] { win->on_translate_clicked(); });
        if (!state.text_only) add(menu, shortcut_text(pin_tr("Pin as text"), "inapp_pin_text_mode"), [win] { win->pin_as_text(); });
    }
    // The image's own items are hidden in thumbnail mode; a text pin is not turned.
    if (!is_thumbnail) add(menu, shortcut_text(pin_tr("Reset size"), "inapp_pin_reset_size"), [win] { win->reset_to_original_size(); });
    if (!is_thumbnail && !state.text_only && !is_animation && !is_files) {
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
    if (!is_thumbnail && !is_animation) {
        add_switch(menu, shortcut_text(pin_tr("Toolbar"), "inapp_toggle_toolbar"), state.toolbar_visible, [win] { win->toggle_toolbar(); });
        add_switch(menu, shortcut_text(pin_tr("Hide annotations"), "inapp_pin_hide_annotations"), state.annotations_hidden, [win] { win->toggle_annotations_hidden(); });
    }
    // Available in every mode.
    add_switch(menu, pin_tr("Always on top"), state.stay_on_top, [win] { win->toggle_stay_on_top(); });
    add_switch(menu, pin_tr("Mouse click-through"), state.click_through, [win] { win->toggle_click_through(); });
    add_switch(menu, shortcut_text(pin_tr("Lock"), "inapp_pin_lock"), state.locked, [win] { win->toggle_lock(); });
    if (!is_thumbnail) {
        add_switch(menu, pin_tr("Border"), state.border_enabled, [win] { win->toggle_border_effect(); });
        if (!is_animation && !is_files) add_switch(menu, shortcut_text(pin_tr("Text selection"), "inapp_pin_text_selection"), state.text_selection_enabled, [win] { win->toggle_text_selection(); });
        if (state.has_formatting) add_switch(menu, pin_tr("Keep text formatting"), state.formatted, [win] { win->toggle_text_formatting(); });
    }
    add_switch(menu, shortcut_text(pin_tr("Thumbnail mode"), "inapp_thumbnail"), state.thumbnail_mode, [win] { win->toggle_thumbnail_mode(); });
    menu->addSeparator();
    auto close_label = pin_tr("Close");
    if (state.close_shortcut_enabled) close_label += " (ESC)";
    add(menu, close_label, [win] { win->close_window(); });
}
}
