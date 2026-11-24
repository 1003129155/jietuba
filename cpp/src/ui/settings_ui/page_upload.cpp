#include "core/ui_scale.h"
#include "settings/tool_settings.h"
#include "ui/dialogs.h"
#include "ui/fluent_lite/buttons.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/settings_ui/components.h"
#include "ui/settings_ui/dialog.h"
#include "ui/settings_ui/pages.h"
#include "ui/settings_ui/upload_target_dialog.h"
#include "upload/s3_target.h"
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>

namespace jietuba {
namespace {
const char *const DEFAULT_KEY = "upload_default_target";
const char *const FORMAT_KEY = "upload_link_format";

IconSource upload_icon() { return icon_ref(QString::fromUtf8("上传.svg")); }

SettingsCardGroup *build_group(SettingsDialog *dialog, QWidget *view);

// Replaces the group by one built from the settings: the list of targets is a column of cards, which the
// group cannot take apart, so a change builds it again.
void rebuild_now(SettingsDialog *dialog) {
    auto *old = dialog->attr<SettingsCardGroup>("upload_group");
    if (!old) return;
    auto *view = old->parentWidget();
    auto *layout = view ? qobject_cast<QVBoxLayout *>(view->layout()) : nullptr;
    if (!layout) return;
    const int index = layout->indexOf(old);
    // The old combos leave the list of controls that are saved and reset with the page.
    std::erase_if(dialog->behavior_controls, [](const auto &entry) { return entry.first == DEFAULT_KEY || entry.first == FORMAT_KEY; });
    auto *group = build_group(dialog, view);
    layout->removeWidget(old);
    layout->insertWidget(index, group);
    old->hide();
    old->deleteLater();
    dialog->adopt_controls(group);
    group->show();
    group->refreshHeight();
    for (const char *key : {DEFAULT_KEY, FORMAT_KEY})
        if (auto *combo = qobject_cast<QComboBox *>(dialog->behavior_control(key))) dialog->update_snapshot(key, combo->currentData());
}

// The card that was clicked is part of the old group, so the rebuild waits for its handler to return.
void rebuild_later(SettingsDialog *dialog) {
    const QPointer<SettingsDialog> guard(dialog);
    QTimer::singleShot(0, dialog, [guard] {
        if (guard) rebuild_now(guard);
    });
}

void write_targets(SettingsDialog *dialog, const std::vector<upload::Target> &targets, const QString &default_id) {
    dialog->config_manager->set_upload_targets(targets.empty() ? QString() : upload::serialize_targets(targets));
    dialog->config_manager->set_upload_default_target(default_id);
}

// An empty id adds a target; the first one added becomes the default.
void edit_target(SettingsDialog *dialog, const QString &id) {
    auto *config = dialog->config_manager;
    auto targets = upload::parse_targets(config->get_upload_targets());
    const bool is_new = id.isEmpty();
    upload::Target initial;
    initial.id = id;
    if (is_new) initial.id = upload::new_target_id();
    else if (const auto *existing = upload::find_target(targets, id)) initial = *existing;
    else return;
    std::vector<upload::Target> others;
    for (const auto &target : targets)
        if (target.id != initial.id) others.push_back(target);
    UploadTargetDialog editor(initial, others, is_new, dialog);
    if (exec_dialog(&editor, dialog) != QDialog::Accepted) return;
    const auto edited = editor.target();
    QString default_id = config->get_upload_default_target();
    if (is_new) {
        if (targets.empty()) default_id = edited.id;
        targets.push_back(edited);
    } else {
        for (auto &target : targets)
            if (target.id == edited.id) target = edited;
    }
    write_targets(dialog, targets, default_id);
    rebuild_later(dialog);
}

void delete_target(SettingsDialog *dialog, const QString &id) {
    auto *config = dialog->config_manager;
    auto targets = upload::parse_targets(config->get_upload_targets());
    const auto *target = upload::find_target(targets, id);
    if (!target) return;
    if (!show_confirm_dialog(dialog, dialog->tr_text("Delete Upload Target"), dialog->tr_text("Delete the upload target \"%1\"?").replace("%1", target->name))) return;
    QString default_id = config->get_upload_default_target();
    if (default_id == id) default_id.clear();
    std::erase_if(targets, [&id](const upload::Target &entry) { return entry.id == id; });
    write_targets(dialog, targets, default_id);
    rebuild_later(dialog);
}

SettingsCardGroup *build_group(SettingsDialog *dialog, QWidget *view) {
    auto *config = dialog->config_manager;
    const auto targets = upload::parse_targets(config->get_upload_targets());
    auto *group = new SettingsCardGroup(dialog->tr_text("Upload"), view, "capture");
    dialog->set_attr("upload_group", group);
    group->setProperty("targets_json", config->get_upload_targets());

    auto *default_card = new SettingCard(upload_icon(), dialog->tr_text("Default Upload Target"),
                                         dialog->tr_text("The target the upload buttons and the upload shortcut send to."), group);
    std::vector<std::pair<QString, QVariant>> choices = {{dialog->tr_text("None"), QString()}};
    for (const auto &target : targets) choices.emplace_back(target.name, target.id);
    auto *default_combo = choice_combo(dialog, default_card, DEFAULT_KEY, choices);
    default_combo->setObjectName("upload_default_combo");
    dialog->set_attr("upload_default_combo", default_combo);
    group->addSettingCard(default_card);

    auto *list_card = new SettingCard(upload_icon(), dialog->tr_text("Upload Targets"),
                                      dialog->tr_text("S3-compatible storage such as AWS S3, Cloudflare R2, Alibaba Cloud OSS, Tencent Cloud COS or MinIO."), group);
    auto *add_button = new PushButton(dialog->tr_text("Add"), list_card);
    add_button->setObjectName("upload_add_button");
    adjust_button_width(add_button, 60);
    QObject::connect(add_button, &QPushButton::clicked, dialog, [dialog] { edit_target(dialog, {}); });
    list_card->addControl(add_button);
    group->addSettingCard(list_card);
    for (const auto &target : targets) {
        // Bucket and host; a host has no spaces, so it may break after its dots instead of running past the buttons.
        const QString where = target.bucket + " · " + QUrl(target.endpoint).host().replace('.', QString(".") + QChar(0x200B));
        auto *card = new SettingCard(upload_icon(), target.name, where, group);
        card->setObjectName("upload_target_" + target.id);
        const QString id = target.id;
        auto *edit = new PushButton(dialog->tr_text("Edit"), card);
        edit->setObjectName("upload_edit_" + id);
        auto *remove = new PushButton(dialog->tr_text("Delete"), card);
        remove->setObjectName("upload_delete_" + id);
        for (auto *button : {edit, remove}) adjust_button_width(button, 60);
        QObject::connect(edit, &QPushButton::clicked, dialog, [dialog, id] { edit_target(dialog, id); });
        QObject::connect(remove, &QPushButton::clicked, dialog, [dialog, id] { delete_target(dialog, id); });
        card->addControl(edit);
        card->addControl(remove);
        group->addSettingCard(card);
    }

    auto *format_card = new SettingCard(upload_icon(), dialog->tr_text("Link Format"), dialog->tr_text("What is copied after an upload."), group);
    auto *format_combo = choice_combo(dialog, format_card, FORMAT_KEY,
                                      {{dialog->tr_text("Link"), QString("url")}, {dialog->tr_text("Markdown"), QString("markdown")}, {dialog->tr_text("HTML"), QString("html")}});
    format_combo->setObjectName("upload_format_combo");
    dialog->set_attr("upload_format_combo", format_combo);
    group->addSettingCard(format_card);
    return group;
}
}

void add_upload_group(SettingsDialog *dialog, QWidget *view, QVBoxLayout *layout) { layout->addWidget(build_group(dialog, view)); }

void refresh_upload_group(SettingsDialog *dialog) {
    // The list of cards is only built again when the targets are not the ones it shows.
    const auto *group = dialog->attr<SettingsCardGroup>("upload_group");
    if (group && group->property("targets_json").toString() != dialog->config_manager->get_upload_targets()) rebuild_now(dialog);
}
}
