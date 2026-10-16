#include "core/logger.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "settings/tool_settings.h"
#include "ui/dialogs.h"
#include "ui/fluent_lite/buttons.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/settings_ui/components.h"
#include "ui/settings_ui/dialog.h"
#include "ui/settings_ui/pages.h"
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QUrl>

namespace jietuba {
namespace {
void open_latest_log_file(SettingsDialog *dialog) {
    const QString path = dialog->config_manager->get_log_dir();
    QDir().mkpath(path);
    const auto files = QDir(path).entryInfoList({"runtime_*.log"}, QDir::Files);
    if (files.isEmpty()) {
        show_info_dialog(dialog, dialog->tr_text("Log"), dialog->tr_text("No log files yet. Please start and use the app first."));
        return;
    }
    const auto latest = *std::max_element(files.begin(), files.end(), [](const QFileInfo &a, const QFileInfo &b) { return a.lastModified() < b.lastModified(); });
    QDesktopServices::openUrl(QUrl::fromLocalFile(latest.absoluteFilePath()));
}
}

void refresh_latest_log_label(SettingsDialog *dialog) {
    auto *label = dialog->attr<QLabel>("latest_log_lbl");
    if (!label) return;
    const QString log_path = get_logger().log_file_path();
    if (!log_path.isEmpty()) label->setText(dialog->tr_text("Current Log:") + " " + QDir::toNativeSeparators(log_path));
    else label->setText(dialog->tr_text("Current Log: (Not generated)  *Log will be created after app starts"));
}

QWidget *create_log_page(SettingsDialog *dialog) {
    auto *config = dialog->config_manager;
    auto *scroll = page_scroll_area(dialog);
    scroll->setWidgetResizable(true);
    scroll->setStyleSheet("QScrollArea { border: none; background: transparent; }");
    auto *view = new QWidget;
    set_own_style(view, "background: transparent;");
    auto *layout = new QVBoxLayout(view);
    layout->setContentsMargins(0, 0, dialog_scaled(10), 0);
    layout->setSpacing(dialog_scaled(20));

    // ════ Log settings ════
    auto *grp_log = new SettingsCardGroup(dialog->tr_text("Log Settings"), view);
    auto *log_card = new SwitchSettingCard(FluentIcon::History, dialog->tr_text("Save Logs"), dialog->tr_text("Saves app activity logs to file."), grp_log);
    log_card->setChecked(config->get_log_enabled());
    dialog->set_attr("log_toggle", log_card);
    grp_log->addSettingCard(log_card);

    auto *level_card = new SettingCard(FluentIcon::Filter, dialog->tr_text("Log Level"), dialog->tr_text("Minimum log level to record"), grp_log);
    auto *level_combo = new ComboBox(level_card);
    const QStringList levels{"DEBUG", "INFO", "WARNING", "ERROR"};
    for (const auto &level : levels) level_combo->addItem(level, level);
    const int level_idx = levels.indexOf(config->get_log_level());
    level_combo->setCurrentIndex(level_idx >= 0 ? level_idx : 2);
    dialog->set_attr("log_level_combo", level_combo);
    level_card->addControl(level_combo);
    grp_log->addSettingCard(level_card);

    auto *retention_card = new SettingCard(FluentIcon::DateTime, dialog->tr_text("Retention Period"), dialog->tr_text("Auto-delete old logs after the selected period"), grp_log);
    auto *retention_combo = new ComboBox(retention_card);
    for (int days : {3, 7, 14, 28, 48}) retention_combo->addItem(QString("%1 %2").arg(days).arg(dialog->tr_text("days")), days);
    int retention_idx = retention_combo->findData(config->get_log_retention_days());
    if (retention_idx < 0) retention_idx = retention_combo->findData(7);
    retention_combo->setCurrentIndex(std::max(retention_idx, 0));
    dialog->set_attr("log_retention_combo", retention_combo);
    retention_card->addControl(retention_combo);
    grp_log->addSettingCard(retention_card);
    layout->addWidget(grp_log);

    // ════ Storage ════
    auto *grp_path = new SettingsCardGroup(dialog->tr_text("Storage"), view);
    auto *path_card = new WhiteCard(grp_path);
    auto *path_v = new QVBoxLayout(path_card);
    path_v->setContentsMargins(dialog_scaled(20), dialog_scaled(14), card_right_margin(), dialog_scaled(14));
    path_v->setSpacing(dialog_scaled(8));
    auto *path_title = new QLabel(dialog->tr_text("Save Location:"), path_card);
    apply_theme_text_style(path_title, 13, true);
    path_v->addWidget(path_title);
    auto *path_lbl = new QLabel(config->get_log_dir(), path_card);
    path_lbl->setWordWrap(true);
    apply_theme_text_style(path_lbl, 13, false, {}, true);
    dialog->set_attr("path_lbl", path_lbl);
    path_v->addWidget(path_lbl);
    auto *latest_log_lbl = new QLabel("", path_card);
    apply_theme_text_style(latest_log_lbl, 13, false, {}, true);
    latest_log_lbl->setWordWrap(true);
    dialog->set_attr("latest_log_lbl", latest_log_lbl);
    refresh_latest_log_label(dialog);
    path_v->addWidget(latest_log_lbl);

    auto *btn_row = new QHBoxLayout;
    btn_row->setSpacing(dialog_scaled(8));
    btn_row->addStretch(1);
    auto *btn_change = new PushButton(dialog->tr_text("Change"), path_card);
    btn_change->setFixedHeight(dialog_scaled(32));
    QObject::connect(btn_change, &QPushButton::clicked, dialog, &SettingsDialog::change_log_dir);
    btn_row->addWidget(btn_change);
    auto *btn_open = new PushButton(dialog->tr_text("Open"), path_card);
    btn_open->setFixedHeight(dialog_scaled(32));
    QObject::connect(btn_open, &QPushButton::clicked, dialog, &SettingsDialog::open_log_dir);
    btn_row->addWidget(btn_open);
    auto *btn_latest = new PushButton(dialog->tr_text("Latest Log"), path_card);
    btn_latest->setFixedHeight(dialog_scaled(32));
    QObject::connect(btn_latest, &QPushButton::clicked, dialog, [dialog] { open_latest_log_file(dialog); });
    btn_row->addWidget(btn_latest);
    path_v->addLayout(btn_row);
    path_card->setFixedHeight(dialog_scaled(146));
    grp_path->addSettingCard(path_card);
    layout->addWidget(grp_path);

    layout->addStretch();
    scroll->setWidget(view);
    return scroll;
}
}
