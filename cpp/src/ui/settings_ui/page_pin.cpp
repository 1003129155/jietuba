#include "core/platform_utils.h"
#include "core/resource_manager.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "ocr/ocr_manager.h"
#include "settings/tool_settings.h"
#include "ui/fluent_lite/buttons.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/settings_ui/components.h"
#include "ui/settings_ui/dialog.h"
#include "ui/settings_ui/pages.h"
#include <cmath>

namespace jietuba {
namespace {
// Combo steps (percent).
const int DEFAULT_OPACITY_STEPS[] = {100, 90, 80, 70, 60, 50, 40, 30};
const int CLICK_THROUGH_OPACITY_STEPS[] = {90, 80, 70, 60, 50, 40, 30, 20};
const int ZOOM_STEPS[] = {5, 10, 15, 20, 25};
const int FINE_ZOOM_STEPS[] = {1, 2, 3, 5};
const int OPACITY_STEPS[] = {1, 2, 5, 10};
const std::pair<const char *, const char *> ZOOM_ANCHOR_LABELS[] = {
    {"top_left", "Top-left"}, {"top_right", "Top-right"}, {"bottom_left", "Bottom-left"},
    {"bottom_right", "Bottom-right"}, {"center", "Center"}, {"mouse", "Mouse Position"},
};

double round2(double value) { return std::round(value * 100) / 100; }

// A combo of (label, stored value); a stored value outside the options shows the default.
ComboBox *choice_combo(SettingsDialog *dialog, SettingCard *card, const QString &key, const std::vector<std::pair<QString, QVariant>> &items,
                       const std::optional<QVariant> &current = std::nullopt) {
    auto *config = dialog->config_manager;
    auto *combo = new ComboBox(card);
    for (const auto &[label, value] : items) combo->addItem(label, value);
    int index = combo->findData(current ? *current : config->get_app_setting(key));
    if (index < 0) index = combo->findData(config->app_defaults().value(key));
    combo->setCurrentIndex(std::max(0, index));
    dialog->behavior_controls.emplace_back(key, combo);
    card->addControl(combo);
    return combo;
}

template <size_t N>
void opacity_combo(SettingsDialog *dialog, SettingCard *card, const QString &key, const int (&steps)[N]) {
    std::vector<std::pair<QString, QVariant>> items;
    for (int percent : steps) items.emplace_back(QString("%1%").arg(percent), round2(percent / 100.0));
    choice_combo(dialog, card, key, items, QVariant(round2(dialog->config_manager->get_app_setting(key).toDouble())));
}

template <size_t N>
void percent_card(SettingsDialog *dialog, SettingsCardGroup *grp, FluentIcon icon, const char *title, const char *desc, const QString &key, const int (&steps)[N]) {
    auto *card = new SettingCard(icon, dialog->tr_text(title), dialog->tr_text(desc), grp);
    std::vector<std::pair<QString, QVariant>> items;
    for (int percent : steps) items.emplace_back(QString("%1%").arg(percent), percent);
    choice_combo(dialog, card, key, items);
    grp->addSettingCard(card);
}

void add_switch_card(SettingsDialog *dialog, SettingsCardGroup *grp, const IconSource &icon, const char *title, const char *desc, const QString &key) {
    auto *card = new SwitchSettingCard(icon, dialog->tr_text(title), dialog->tr_text(desc), grp);
    card->setChecked(dialog->config_manager->get_app_setting(key).toBool());
    dialog->behavior_controls.emplace_back(key, card);
    grp->addSettingCard(card);
}

void build_behavior_section(SettingsDialog *dialog, SettingsCardGroup *grp) {
    auto *toolbar_card = new SwitchSettingCard(FluentIcon::Pin, dialog->tr_text("Auto-show Drawing Tools on Pin"),
                                               dialog->tr_text("On: Shows toolbar when mouse enters pinned window.") + "\n" +
                                                   dialog->tr_text("Off: Show via right-click toolbar button."),
                                               grp);
    toolbar_card->setChecked(dialog->config_manager->get_pin_auto_toolbar());
    dialog->set_attr("pin_auto_toolbar_toggle", toolbar_card);
    grp->addSettingCard(toolbar_card);

    bool ocr_available = false;
    try {
        ocr_available = !get_available_engines().isEmpty();
    } catch (const std::exception &) {
        ocr_available = false;
    }
    auto *ocr_card = new SwitchSettingCard(FluentIcon::Font, dialog->tr_text("Automatically recognize text after pinning"),
                                           dialog->tr_text("On very low-end computers, OCR after pinning may cause a brief stutter. Turn off automatic recognition if needed."),
                                           grp);
    ocr_card->setChecked(ocr_available ? dialog->config_manager->get_ocr_enabled() : false);
    ocr_card->setEnabled(ocr_available);
    dialog->set_attr("ocr_enable_toggle", ocr_card);
    grp->addSettingCard(ocr_card);

    auto *opacity_card = new SettingCard(FluentIcon::Transparent, dialog->tr_text("Default Opacity"),
                                         dialog->tr_text("Opacity of newly pinned images. Each image can still be adjusted with the mouse."), grp);
    opacity_combo(dialog, opacity_card, "pin_default_opacity", DEFAULT_OPACITY_STEPS);
    grp->addSettingCard(opacity_card);
}

// Each wheel notch adds or removes a fixed percentage of the original size; step, anchor and edge dragging are here.
void build_zoom_section(SettingsDialog *dialog, SettingsCardGroup *grp) {
    percent_card(dialog, grp, FluentIcon::Search, "Zoom Step", "How much one wheel notch zooms, relative to the original size.", "pin_zoom_step", ZOOM_STEPS);
    percent_card(dialog, grp, FluentIcon::Search, "Fine Zoom Step", "Zoom step for the fine zoom mouse shortcut (Alt + wheel by default).", "pin_fine_zoom_step",
                 FINE_ZOOM_STEPS);
    percent_card(dialog, grp, FluentIcon::Transparent, "Opacity Step", "Opacity change per notch of the opacity mouse shortcut (Ctrl + wheel by default).",
                 "pin_opacity_step", OPACITY_STEPS);
    auto *anchor_card = new SettingCard(FluentIcon::Alignment, dialog->tr_text("Zoom Anchor"),
                                        dialog->tr_text("The point of the image that stays in place while zooming with the wheel."), grp);
    std::vector<std::pair<QString, QVariant>> anchors;
    for (const auto &[value, label] : ZOOM_ANCHOR_LABELS) anchors.emplace_back(dialog->tr_text(label), QString(value));
    choice_combo(dialog, anchor_card, "pin_zoom_anchor", anchors);
    grp->addSettingCard(anchor_card);
    add_switch_card(dialog, grp, FluentIcon::Layout, "Resize by Dragging Edges",
                    "Drag near the edge or corner of a pinned image to resize it. The aspect ratio is kept.", "pin_border_resize");
    add_switch_card(dialog, grp, FluentIcon::FontSize, "Shrink Oversized Images", "Images larger than the screen are scaled down to fit and centered when pinned.",
                    "pin_auto_shrink");
}

// System rounded corners and shadow, new pins' border, hover buttons. Corners and buttons
// apply to open pins after saving; the border only decides new pins, existing ones toggle
// it from their context menu.
void build_appearance_section(SettingsDialog *dialog, SettingsCardGroup *grp) {
    auto *config = get_tool_settings_manager();
    // Rounded corners and the shadow came with Windows 11; earlier systems get no switch.
    if (supports_window_corner_preference()) {
        auto *corners_card = new SwitchSettingCard(FluentIcon::Layout, dialog->tr_text("Rounded Corners and Shadow"),
                                                   dialog->tr_text("Windows 11 rounds the corners of pinned images and adds a shadow. "
                                                                   "Turn off to show the whole image with square corners and no shadow."),
                                                   grp);
        corners_card->setChecked(config->get_app_setting("pin_rounded_corners").toBool());
        dialog->behavior_controls.emplace_back("pin_rounded_corners", corners_card);
        grp->addSettingCard(corners_card);
    }
    auto *border_card = new SwitchSettingCard(FluentIcon::Palette, dialog->tr_text("Auto Border"),
                                              dialog->tr_text("New pinned images get a theme-colored border. Each image can still toggle it from the right-click menu."),
                                              grp);
    border_card->setChecked(config->get_app_setting("pin_auto_border").toBool());
    dialog->behavior_controls.emplace_back("pin_auto_border", border_card);
    grp->addSettingCard(border_card);
    auto *buttons_card = new SwitchSettingCard(FluentIcon::Close, dialog->tr_text("Hover Buttons"),
                                               dialog->tr_text("Show the close and toolbar buttons in the top-right corner when the mouse "
                                                               "is over a pinned image. When off, use the right-click menu or shortcuts."),
                                               grp);
    buttons_card->setChecked(config->get_app_setting("pin_hover_buttons").toBool());
    dialog->behavior_controls.emplace_back("pin_hover_buttons", buttons_card);
    grp->addSettingCard(buttons_card);
}

void build_click_through_section(SettingsDialog *dialog, SettingsCardGroup *grp) {
    auto *card = new SettingCard(FluentIcon::Transparent, dialog->tr_text("Click-Through Opacity"),
                                 dialog->tr_text("Opacity of a pinned image when click-through starts from its right-click menu. "
                                                 "The slider on the bar above it still adjusts each image."),
                                 grp);
    opacity_combo(dialog, card, "pin_click_through_opacity", CLICK_THROUGH_OPACITY_STEPS);
    grp->addSettingCard(card);
}

// What a lock blocks. The context menu is never blocked: it unlocks and runs explicit actions.
void build_lock_section(SettingsDialog *dialog, SettingsCardGroup *grp) {
    add_switch_card(dialog, grp, ResourceManager::get_icon_path(QString::fromUtf8("移动窗口.svg")), "Block Moving and Resizing",
                    "Locked images cannot be dragged, zoomed or resized.", "pin_lock_block_move");
    add_switch_card(dialog, grp, FluentIcon::Close, "Block Closing by Mouse", "The close button and the close mouse shortcut do nothing on locked images.",
                    "pin_lock_block_click_close");
    add_switch_card(dialog, grp, FluentIcon::CommandPrompt, "Block Closing by Keyboard", "Esc does nothing on locked images.", "pin_lock_block_key_close");
    add_switch_card(dialog, grp, FluentIcon::Info, "Show Lock Hints",
                    "Show a short hint when locking, unlocking or trying a blocked action. The right-click menu always works on locked images.", "pin_lock_hint");
}

// Recording and conflict checks live on the shortcut pages; this has only entries, so there is one editor of each.
void build_shortcuts_section(SettingsDialog *dialog, SettingsCardGroup *grp) {
    const std::tuple<IconSource, const char *, const char *, bool> entries[] = {
        {FluentIcon::CommandPrompt, "Keyboard Shortcuts", "Copy, lock, thumbnail, toolbar and other keys for pinned images.", false},
        {ResourceManager::get_icon_path(QString::fromUtf8("鼠标线框.svg")), "Mouse Shortcuts", "Zoom, opacity, close and other mouse actions for pinned images.", true},
    };
    for (const auto &[icon, title, desc, mouse] : entries) {
        auto *card = new SettingCard(icon, dialog->tr_text(title), dialog->tr_text(desc), grp);
        auto *button = new PushButton(dialog->tr_text("Edit"), card);
        const bool is_mouse = mouse;
        QObject::connect(button, &QPushButton::clicked, dialog, [dialog, is_mouse] { dialog->show_pin_shortcuts(is_mouse); });
        card->addControl(button);
        grp->addSettingCard(card);
    }
}
}

const std::set<QString> PIN_BEHAVIOR_KEYS = {
    "pin_default_opacity", "pin_rounded_corners", "pin_auto_border", "pin_hover_buttons", "pin_click_through_opacity",
    "pin_zoom_step", "pin_fine_zoom_step", "pin_opacity_step", "pin_zoom_anchor", "pin_border_resize", "pin_auto_shrink",
    "pin_lock_block_move", "pin_lock_block_click_close", "pin_lock_block_key_close", "pin_lock_hint",
};

// Pin settings: behaviour, zoom and size, appearance, click-through, lock; shortcuts jump to the shortcut pages' pin tab.
QWidget *create_pin_page(SettingsDialog *dialog) {
    auto *scroll = page_scroll_area(dialog);
    scroll->setWidgetResizable(true);
    scroll->setStyleSheet("QScrollArea { border: none; background: transparent; }");
    auto *view = new QWidget;
    set_own_style(view, "background: transparent;");
    auto *layout = new QVBoxLayout(view);
    layout->setContentsMargins(0, 0, dialog_scaled(10), 0);
    layout->setSpacing(dialog_scaled(14));
    const std::pair<const char *, void (*)(SettingsDialog *, SettingsCardGroup *)> sections[] = {
        {"Behavior", build_behavior_section},
        {"Zoom and Size", build_zoom_section},
        {"Appearance", build_appearance_section},
        {"Mouse Click-Through", build_click_through_section},
        {"Lock", build_lock_section},
        {"Shortcuts", build_shortcuts_section},
    };
    for (const auto &[title, build] : sections) {
        auto *group = new SettingsCardGroup(dialog->tr_text(title), view, "pin");
        build(dialog, group);
        layout->addWidget(group);
    }
    layout->addStretch();
    scroll->setWidget(view);
    return scroll;
}
}
