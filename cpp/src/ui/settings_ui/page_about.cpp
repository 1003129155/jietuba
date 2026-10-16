#include "core/constants.h"
#include "core/logger.h"
#include "core/update_checker.h"
#include "core/update_controller.h"
#include "core/updater_process.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "main_app.h"
#include "ui/dialogs.h"
#include "ui/fluent_lite/buttons.h"
#include "ui/settings_ui/components.h"
#include "ui/settings_ui/dialog.h"
#include "ui/settings_ui/pages.h"

namespace jietuba {
QWidget *create_about_page(SettingsDialog *dialog) {
    auto *scroll = page_scroll_area(dialog);
    scroll->setWidgetResizable(true);
    scroll->setStyleSheet("QScrollArea { border: none; background: transparent; }");
    auto *view = new QWidget;
    set_own_style(view, "background: transparent;");
    auto *layout = new QVBoxLayout(view);
    layout->setContentsMargins(0, dialog_scaled(20), dialog_scaled(10), dialog_scaled(20));
    layout->setSpacing(dialog_scaled(16));

    auto *group = new SettingsCardGroup(dialog->tr_text("About"), view);
    auto *name_card = new SettingCard(FluentIcon::Application, dialog->tr_text("Version"), APP_VERSION, group);
    // Builds without the bundled updater helper (JIETUBA_UPDATES off) offer no update check.
    if (updater_bundled()) {
        auto *check_update_btn = new PushButton(dialog->tr_text("Check for Updates"), name_card);
        check_update_btn->setObjectName("checkUpdateButton");
        name_card->addControl(check_update_btn);
        auto *release_checker = new GitHubReleaseChecker(dialog);
        auto finish_check = [dialog, check_update_btn] {
            check_update_btn->setEnabled(true);
            check_update_btn->setText(dialog->tr_text("Check for Updates"));
        };
        auto show_check_error = [dialog, finish_check](const QString &reason) {
            finish_check();
            log_warning(T("检查更新失败: {reason}", {{"reason", reason}}), "Update");
            show_warning_dialog(dialog, dialog->tr_text("Update Check Failed"),
                                dialog->tr_text("Unable to check for updates. Please check your network connection and try again."));
        };
        QObject::connect(release_checker, &GitHubReleaseChecker::release_found, dialog, [dialog, finish_check, show_check_error](const ReleaseInfo &release) {
            bool available = false;
            try {
                available = release.available.has_value() ? *release.available : is_newer_version(release.tag_name, APP_VERSION);
            } catch (const UpdateCheckError &e) {
                show_check_error(QString::fromStdString(e.what()));
                return;
            }
            finish_check();
            if (!available) {
                show_info_dialog(dialog, dialog->tr_text("Update Check"), dialog->tr_text("You're using the latest version (%1).").replace("%1", APP_VERSION));
                return;
            }
            auto *controller = qobject_cast<UpdateController *>(qApp->property("_update_controller").value<QObject *>());
            if (controller && !release.asset_name.isEmpty()) {
                controller->present(release);
                return;
            }
            const QString notes = release.notes.isEmpty() ? dialog->tr_text("No release notes provided.") : release.notes;
            const QString content = QStringList{dialog->tr_text("Current version: %1").replace("%1", APP_VERSION),
                                                dialog->tr_text("Latest version: %1").replace("%1", release.tag_name), QString(),
                                                dialog->tr_text("What's new:"), notes}
                                        .join('\n');
            show_update_dialog(dialog, dialog->tr_text("Update Available"), content, dialog->tr_text("Download:"), release.url,
                               dialog->tr_text("Open Download Page"));
        });
        QObject::connect(release_checker, &GitHubReleaseChecker::failed, dialog, show_check_error);
        QObject::connect(check_update_btn, &QPushButton::clicked, dialog, [dialog, release_checker, check_update_btn] {
            if (!release_checker->check()) return;
            check_update_btn->setEnabled(false);
            check_update_btn->setText(dialog->tr_text("Checking..."));
        });
    }
    group->addSettingCard(name_card);
    group->addSettingCard(new SettingCard(FluentIcon::People, dialog->tr_text("Developer"), "JYAARU", group));

    auto *license_card = new SettingCard(FluentIcon::Document, dialog->tr_text("Licenses"), "MIT", group);
    auto *details_btn = new HyperlinkButton("", dialog->tr_text("Details"), license_card);
    QObject::connect(details_btn, &QPushButton::clicked, dialog, [dialog] { show_text_dialog(dialog, dialog->tr_text("MIT License"), dialog->tr_text("MIT License Text")); });
    license_card->addControl(details_btn, Qt::AlignLeft);
    group->addSettingCard(license_card);

    auto *github_card = new SettingCard(FluentIcon::GitHub, dialog->tr_text("Source Code"), "github.com/1003129155/jietuba", group);
    auto *link_btn = new HyperlinkButton(PROJECT_GITHUB_URL, dialog->tr_text("Open GitHub"), github_card);
    github_card->addControl(link_btn, Qt::AlignLeft);
    group->addSettingCard(github_card);

    layout->addWidget(group);
    layout->addStretch(1);
    scroll->setWidget(view);
    return scroll;
}
}
