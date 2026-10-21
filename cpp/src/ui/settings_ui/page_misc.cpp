#include "core/elevation.h"
#include "core/i18n.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "main_app.h"
#include "settings/settings_transfer.h"
#include "settings/tool_settings.h"
#include "ui/dialogs.h"
#include "ui/fluent_lite/buttons.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/labels.h"
#include "ui/settings_ui/components.h"
#include "ui/settings_ui/dialog.h"
#include "ui/settings_ui/pages.h"
#include <QDate>
#include <QDir>
#include <QStandardPaths>

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
    card->setChecked(dialog->config_manager->get_app_setting("run_as_admin").toBool());
    dialog->set_attr("run_as_admin_toggle", card);
    group->addSettingCard(card);
    QObject::connect(autostart_card, &SwitchSettingCard::checkedChanged, group, [group, card](bool checked) { group->setCardVisible(card, checked); });
    group->setCardVisible(card, autostart_card->isChecked());
}

void export_settings_to_file(SettingsDialog *dialog) {
    // Export writes the saved settings, so edits still waiting to be saved go first.
    dialog->apply_settings();
    const QString title = dialog->tr_text("Export Settings");
    const QString suggested = QDir(documents_dir()).filePath(QString("jietuba-settings-%1.json").arg(QDate::currentDate().toString("yyyyMMdd")));
    const QString path = get_save_file_name(dialog, title, suggested, dialog->tr_text("Settings File (*.json)")).first;
    if (path.isEmpty()) return;
    try {
        const auto keys = dialog->settings_keys();
        settings_transfer::export_settings(dialog->config_manager, QSet<QString>(keys.begin(), keys.end()), path, APP_VERSION);
    } catch (const std::runtime_error &e) {
        show_error_dialog(dialog, title, dialog->tr_text("Could not write the file: %1").replace("%1", QString::fromUtf8(e.what())));
        return;
    }
    show_info_dialog(dialog, title, dialog->tr_text("Settings exported to %1").replace("%1", path));
}

void import_settings_from_file(SettingsDialog *dialog) {
    const QString title = dialog->tr_text("Import Settings");
    const QString path = get_open_file_name(dialog, title, documents_dir(), dialog->tr_text("Settings File (*.json)")).first;
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
    autostart_card->setChecked(config->get_app_setting("autostart_enabled").toBool());
    dialog->set_attr("autostart_toggle", autostart_card);
    grp_startup->addSettingCard(autostart_card);
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
    if (const int index = language_combo->findData(config->get_app_setting("language", "ja")); index >= 0) language_combo->setCurrentIndex(index);
    dialog->set_attr("language_combo", language_combo);
    lang_card->addControl(language_combo);
    grp_ops->addSettingCard(lang_card);
    layout->addWidget(grp_ops);

    // ════ Settings backup ════
    auto *grp_backup = new SettingsCardGroup(dialog->tr_text("Settings Backup"), view);
    auto *export_card = new SettingCard(FluentIcon::Save, dialog->tr_text("Export Settings"),
                                        dialog->tr_text("Save settings to a file, including translation API keys. Folder locations are not included."), grp_backup);
    auto *export_button = new PushButton(dialog->tr_text("Export"), export_card);
    QObject::connect(export_button, &QPushButton::clicked, dialog, [dialog] { export_settings_to_file(dialog); });
    dialog->set_attr("export_settings_button", export_button);
    export_card->addControl(export_button);
    grp_backup->addSettingCard(export_card);
    auto *import_card = new SettingCard(FluentIcon::Download, dialog->tr_text("Import Settings"),
                                        dialog->tr_text("Load settings from an exported file. Folder locations stay unchanged."), grp_backup);
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
