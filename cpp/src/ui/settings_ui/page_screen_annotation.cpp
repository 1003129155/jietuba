#include "annotation/annotation_toolbar.h"
#include "core/resource_manager.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "settings/tool_settings.h"
#include "ui/fluent_lite/buttons.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/settings_ui/components.h"
#include "ui/settings_ui/dialog.h"
#include "ui/settings_ui/pages.h"

namespace jietuba {
namespace {
// A combo of (label, stored value); a stored value outside the options shows the default.
void add_choice_card(SettingsDialog *dialog, SettingsCardGroup *grp, const IconSource &icon, const char *title, const char *desc, const QString &key,
                     const std::vector<std::pair<const char *, const char *>> &items) {
    auto *config = dialog->config_manager;
    auto *card = new SettingCard(icon, dialog->tr_text(title), dialog->tr_text(desc), grp);
    auto *combo = new ComboBox(card);
    for (const auto &[label, value] : items) combo->addItem(dialog->tr_text(label), QString(value));
    int index = combo->findData(config->get_app_setting(key));
    if (index < 0) index = combo->findData(config->app_defaults().value(key));
    combo->setCurrentIndex(std::max(0, index));
    dialog->behavior_controls.emplace_back(key, combo);
    card->addControl(combo);
    grp->addSettingCard(card);
}

void add_switch_card(SettingsDialog *dialog, SettingsCardGroup *grp, const IconSource &icon, const char *title, const char *desc, const QString &key) {
    auto *card = new SwitchSettingCard(icon, dialog->tr_text(title), dialog->tr_text(desc), grp);
    card->setChecked(dialog->config_manager->get_app_setting(key).toBool());
    dialog->behavior_controls.emplace_back(key, card);
    grp->addSettingCard(card);
}

void build_start_section(SettingsDialog *dialog, SettingsCardGroup *grp) {
    add_choice_card(dialog, grp, ResourceManager::get_icon_path(QString::fromUtf8("画笔.svg")), "Start With",
                    "What screen annotation starts with. In pass-through the mouse keeps working the windows below until a tool is picked.",
                    "screen_annotation_start_tool", {{"Pass-Through", "cursor"}, {"Pen", "pen"}, {"Last Used Tool", "last"}});
    add_switch_card(dialog, grp, FluentIcon::Brush, "Keep Screen Annotations",
                    "Ending the screen annotation keeps its drawings; they come back the next time it starts.", "screen_annotation_keep_drawings");
    // The hotkey is recorded and checked for conflicts on the shortcut page; this is an entry to it.
    auto *hotkey_card = new SettingCard(ResourceManager::get_icon_path(QString::fromUtf8("热键.svg")), dialog->tr_text("Global Hotkey"), QString(), grp);
    auto *button = new PushButton(dialog->tr_text("Edit"), hotkey_card);
    QObject::connect(button, &QPushButton::clicked, dialog, [dialog] { dialog->show_hotkey_row("screen_annotation_hotkey_edit"); });
    hotkey_card->addControl(button);
    dialog->set_attr("screen_annotation_hotkey_card", hotkey_card);
    grp->addSettingCard(hotkey_card);
    dialog->refresh_screen_annotation_hotkey();
}

void build_toolbar_section(SettingsDialog *dialog, SettingsCardGroup *grp) {
    add_choice_card(dialog, grp, FluentIcon::Layout, "Toolbar Position", "Where the toolbar appears when screen annotation starts.",
                    "screen_annotation_toolbar_position", {{"Where It Was Last", "last"}, {"Bottom Center of the Screen", "bottom"}, {"Top Center of the Screen", "top"}});
    add_switch_card(dialog, grp, FluentIcon::Camera, "Toolbar Invisible to Screen Capture",
                    "Keeps the toolbar and its panels out of recordings, screenshots, screen sharing, remote desktop and other screen capture; you still see them. Turn off to let them be captured, for example when recording a tutorial.",
                    "screen_annotation_hide_toolbar_in_captures");
    auto *buttons_card = new SettingCard(FluentIcon::Setting, dialog->tr_text("Toolbar Buttons"),
                                         dialog->tr_text("Choose and order the buttons of the screen annotation toolbar. Saved at once."), grp);
    auto *button = new PushButton(dialog->tr_text("Adjust"), buttons_card);
    QObject::connect(button, &QPushButton::clicked, dialog, [dialog] { AnnotationToolbar::edit_layout(dialog); });
    buttons_card->addControl(button);
    grp->addSettingCard(buttons_card);
}

void build_highlight_section(SettingsDialog *dialog, SettingsCardGroup *grp) {
    const IconSource icon = ResourceManager::get_icon_path(QString::fromUtf8("光标高亮.svg"));
    add_switch_card(dialog, grp, icon, "Cursor Highlight at Start",
                    "Starts with a light around the pointer. The toolbar button turns it on and off during a session, and that choice is kept.",
                    "screen_annotation_cursor_highlight");
    add_choice_card(dialog, grp, icon, "Highlight Size", "The size of the light around the pointer and of the click rings.", "screen_annotation_highlight_size",
                    {{"Small", "small"}, {"Medium", "medium"}, {"Large", "large"}});
    add_switch_card(dialog, grp, icon, "Click Rings", "A ring spreads from every click: yellow for the left button, blue for the right.",
                    "screen_annotation_click_ripples");
}
}

const std::set<QString> SCREEN_ANNOTATION_BEHAVIOR_KEYS = {
    "screen_annotation_start_tool",       "screen_annotation_keep_drawings",  "screen_annotation_toolbar_position",
    "screen_annotation_cursor_highlight", "screen_annotation_highlight_size", "screen_annotation_click_ripples", "screen_annotation_hide_toolbar_in_captures",
};

// Screen annotation: how a session starts and ends, its toolbar, the cursor highlight.
QWidget *create_screen_annotation_page(SettingsDialog *dialog) {
    auto *scroll = page_scroll_area(dialog);
    scroll->setWidgetResizable(true);
    scroll->setStyleSheet("QScrollArea { border: none; background: transparent; }");
    auto *view = new QWidget;
    set_own_style(view, "background: transparent;");
    auto *layout = new QVBoxLayout(view);
    layout->setContentsMargins(0, 0, dialog_scaled(10), 0);
    layout->setSpacing(dialog_scaled(14));
    const std::pair<const char *, void (*)(SettingsDialog *, SettingsCardGroup *)> sections[] = {
        {"Start and End", build_start_section},
        {"Toolbar", build_toolbar_section},
        {"Cursor Highlight", build_highlight_section},
    };
    for (const auto &[title, build] : sections) {
        auto *group = new SettingsCardGroup(dialog->tr_text(title), view, "capture");
        build(dialog, group);
        layout->addWidget(group);
    }
    layout->addStretch();
    scroll->setWidget(view);
    return scroll;
}
}
