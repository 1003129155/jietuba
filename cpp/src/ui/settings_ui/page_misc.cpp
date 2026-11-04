#include "core/data_location.h"
#include "core/elevation.h"
#include "core/i18n.h"
#include "core/platform_utils.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "settings/data_folder.h"
#include "settings/settings_transfer.h"
#include "settings/tool_settings.h"
#include "ui/dialogs.h"
#include "ui/fluent_lite/buttons.h"
#include "ui/fluent_lite/cards.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/labels.h"
#include "ui/settings_ui/components.h"
#include "ui/settings_ui/dialog.h"
#include "ui/settings_ui/pages.h"
#include <QDesktopServices>
#include <QDir>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <functional>

namespace jietuba {
namespace {
QString documents_dir() { return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation); }

// Run as administrator, a child of autostart; hidden for accounts that cannot elevate. It can be
// changed without elevation too: applying goes through UAC (SettingsDialog::apply_startup).
void add_run_as_admin_card(SettingsDialog *dialog, SettingsCardGroup *group, SwitchSettingCard *autostart_card) {
    const QString state = elevation::elevation_state();
    if (state == elevation::STANDARD) return;
    QString description = dialog->tr_text("Lets screenshots capture administrator windows such as Task Manager. "
                                          "Launching it by hand also runs as administrator, without a UAC prompt.");
    if (state == elevation::CAN_ELEVATE)
        description += "\n" + dialog->tr_text("Changing it asks for administrator permission. Turning it on restarts the program as administrator.");
    auto *card = new SubSwitchSettingCard(dialog->tr_text("Run as Administrator"), description, group);
    card->setChecked(dialog->config_manager->get_run_as_admin());
    dialog->set_attr("run_as_admin_toggle", card);
    group->addSettingCard(card);
    QObject::connect(autostart_card, &SwitchSettingCard::checkedChanged, group, [group, card](bool checked) { group->setCardVisible(card, checked); });
    group->setCardVisible(card, autostart_card->isChecked());
}

void open_settings_folder(SettingsDialog *dialog) {
    // Opened from the event loop rather than inside the button's mouse release.
    const QString folder = QDir::toNativeSeparators(data_location().dir);
    QTimer::singleShot(0, dialog, [folder] { QDesktopServices::openUrl(QUrl::fromLocalFile(folder)); });
}

// Green mode moves the settings file at once; a failed move leaves the switch where it was.
SwitchSettingCard *add_green_mode_card(SettingsDialog *dialog, SettingsCardGroup *group, std::function<void()> moved) {
    const bool on = data_location().beside_program;
    const bool writable = on || data_folder::folder_writable(data_folder::green_mode_dir());
    QString description = dialog->tr_text("Keep the settings in the program's folder; copy the whole folder to take them along.");
    if (!writable) description += "\n" + dialog->tr_text("The program's folder cannot be written to.");
    auto *card = new SwitchSettingCard(FluentIcon::Application, dialog->tr_text("Green Mode"), description, group);
    card->setChecked(on);
    card->setEnabled(writable);
    QObject::connect(card, &SwitchSettingCard::checkedChanged, dialog, [dialog, card, moved](bool checked) {
        const QString error = data_folder::set_green_mode(*dialog->config_manager, checked);
        if (!error.isEmpty()) {
            const QSignalBlocker block(card);
            card->setChecked(!checked);
            show_warning_dialog(dialog, dialog->tr_text("Error"), dialog->tr_text("Could not move the settings file: %1").replace("%1", error));
            return;
        }
        moved();
    });
    group->addSettingCard(card);
    return card;
}

void import_settings_from_file(SettingsDialog *dialog) {
    const QString title = dialog->tr_text("Import Settings");
    const QString path = get_open_file_name(dialog, title, documents_dir(), dialog->tr_text("Settings File (*.ini *.json)")).first;
    if (path.isEmpty()) return;
    QVariantMap values;
    try {
        values = settings_transfer::read_settings_file(path);
    } catch (const settings_transfer::SettingsFileError &) {
        show_error_dialog(dialog, title, dialog->tr_text("This file is not a Jietuba settings file, or it is damaged."));
        return;
    }
    dialog->load_imported_settings(values);
    show_info_dialog(dialog, title, dialog->tr_text("Settings imported."));
}
}

QWidget *create_misc_page(SettingsDialog *dialog) {
    auto *config = dialog->config_manager;
    auto *scroll = page_scroll_area(dialog);
    scroll->setWidgetResizable(true);
    scroll->setStyleSheet("QScrollArea { border: none; background: transparent; }");
    auto *view = new QWidget;
    set_own_style(view, "background: transparent;");
    auto *layout = new QVBoxLayout(view);
    layout->setContentsMargins(0, 0, dialog_scaled(10), 0);
    layout->setSpacing(dialog_scaled(14));

    // ════ Startup ════
    auto *grp_startup = new SettingsCardGroup(dialog->tr_text("Startup"), view);
    auto *autostart_card = new SwitchSettingCard(FluentIcon::PowerButton, dialog->tr_text("Launch on Startup"), dialog->tr_text("Register in Windows startup via registry."), grp_startup);
    autostart_card->setChecked(config->get_autostart_enabled());
    dialog->set_attr("autostart_toggle", autostart_card);
    grp_startup->addSettingCard(autostart_card);
    // Task Manager's switch is the user's choice in Windows; the card tells about it rather than undoing it.
    dialog->add_external_refresh([] { return autostart_disabled_by_system(); }, [dialog, autostart_card](bool disabled) {
        autostart_card->setContent(disabled
                                       ? dialog->tr_text("Turned off in Task Manager's Startup apps, so Windows will not start it at sign-in. Turn it back on there.")
                                       : dialog->tr_text("Register in Windows startup via registry."));
    });
    add_run_as_admin_card(dialog, grp_startup, autostart_card);
    auto *show_card = new SwitchSettingCard(FluentIcon::Application, dialog->tr_text("Show Main Window on Startup"), dialog->tr_text("If off, starts in background."), grp_startup);
    show_card->setChecked(config->get_show_main_window());
    dialog->set_attr("show_main_window_toggle", show_card);
    grp_startup->addSettingCard(show_card);
    layout->addWidget(grp_startup);

    // ════ Operation ════
    auto *grp_ops = new SettingsCardGroup(dialog->tr_text("Operation"), view);
    auto *lang_card = new SettingCard(FluentIcon::Language, dialog->tr_text("Language"), dialog->tr_text("Select display language. Restart required after change."), grp_ops);
    auto *language_combo = new ComboBox(lang_card);
    for (const auto &[code, name] : I18nManager::language_items()) language_combo->addItem(name, code);
    if (const int index = language_combo->findData(config->get_language("ja")); index >= 0) language_combo->setCurrentIndex(index);
    dialog->set_attr("language_combo", language_combo);
    lang_card->addControl(language_combo);
    grp_ops->addSettingCard(lang_card);
    layout->addWidget(grp_ops);

    // ════ Settings backup ════
    auto *grp_backup = new SettingsCardGroup(dialog->tr_text("Settings Backup"), view);
    // The settings file itself is the backup, so there is no export.
    const auto file_text = [dialog] {
        return QDir::toNativeSeparators(data_location().settings_file()) + "\n" +
               dialog->tr_text("Copy this file to back up your settings or use them on another computer.");
    };
    auto *file_card = new SettingCard(FluentIcon::Folder, dialog->tr_text("Settings File"), file_text(), grp_backup);
    auto *green_card = add_green_mode_card(dialog, grp_backup, [file_card, file_text] { file_card->setContent(file_text()); });
    dialog->set_attr("green_mode_toggle", green_card);
    auto *open_button = new PushButton(dialog->tr_text("Open"), file_card);
    QObject::connect(open_button, &QPushButton::clicked, dialog, [dialog] { open_settings_folder(dialog); });
    dialog->set_attr("open_settings_folder_button", open_button);
    file_card->addControl(open_button);
    grp_backup->addSettingCard(file_card);
    auto *import_card = new SettingCard(FluentIcon::Download, dialog->tr_text("Import Settings"),
                                        dialog->tr_text("Load settings from another settings.ini or a file exported by an earlier version. Folder locations stay unchanged."),
                                        grp_backup);
    auto *import_button = new PushButton(dialog->tr_text("Import"), import_card);
    QObject::connect(import_button, &QPushButton::clicked, dialog, [dialog] { import_settings_from_file(dialog); });
    dialog->set_attr("import_settings_button", import_button);
    import_card->addControl(import_button);
    grp_backup->addSettingCard(import_card);
    layout->addWidget(grp_backup);

    auto *hint = new CaptionLabel(dialog->tr_text("💡 Hint: Even with background startup, you can operate from system tray."), view);
    hint->setStyleSheet(QString("padding: %1px;").arg(dialog_scaled(5)));
    layout->addWidget(hint);
    layout->addStretch();
    scroll->setWidget(view);
    return scroll;
}
}
