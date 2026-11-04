#include "clipboard/core/manager.h"
#include "core/data_location.h"
#include "core/i18n.h"
#include "core/ui_scale.h"
#include "settings/data_folder.h"
#include "settings/tool_settings.h"
#include "ui/dialogs.h"
#include "ui/fluent_lite/buttons.h"
#include "ui/fluent_lite/icons.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/settings_ui/pages.h"
#include "ui/welcome/pages.h"
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QSignalBlocker>

namespace jietuba {
namespace {
QString settings_text(const char *text) { return i18n_tr(text, "SettingsDialog"); }
QString page_subtitle() { return welcome_tr("截图、剪贴板记录和设置各自存在哪里，之后也可以在设置里改。"); }

QString clipboard_folder(const ToolSettingsManager *config) {
    const QString db = clipboard::ClipboardManager::instance().get_db_path().value_or(config->get_clipboard_db_path());
    return QDir::toNativeSeparators(QFileInfo(db).absolutePath());
}

// A path shown in full as a tooltip: the field is narrower than most paths.
LineEdit *path_field(const QString &path, bool editable) {
    auto *edit = new LineEdit;
    edit->setText(path);
    edit->setToolTip(path);
    edit->setReadOnly(!editable);
    edit->setFixedHeight(dialog_scaled(34));
    edit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    return edit;
}

PushButton *path_button(const QString &text, FluentIcon icon) {
    auto *button = new PushButton(text, nullptr, icon);
    button->setFixedHeight(dialog_scaled(34));
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

// The row's title stands alone; its control is the path row under it.
QWidget *no_control() {
    auto *widget = new QWidget;
    widget->setFixedSize(0, 0);
    return widget;
}

void add_under(QWidget *card, QWidget *field, QWidget *button) {
    auto *row = new QHBoxLayout;
    row->setContentsMargins(0, dialog_scaled(6), 0, 0);
    row->setSpacing(dialog_scaled(6));
    row->addWidget(field, 1);
    if (button) row->addWidget(button);
    static_cast<QVBoxLayout *>(card->layout())->addLayout(row);
}
}

StoragePage::StoragePage(ToolSettingsManager *config, QWidget *parent) : BasePage(welcome_tr("存储位置"), page_subtitle(), parent), config_(config) {
    init();
    // All paths: the cards take the page's width, so the paths show more.
    illus_area->hide();
}

IllustrationArea *StoragePage::create_illustration() { return new IllustrationArea(this); }

void StoragePage::build_controls(QVBoxLayout *layout) {
    layout->setSpacing(dialog_scaled(12));
    // ── Screenshots: whether to save, then where. ──
    autosave_toggle_ = new WelcomeToggleSwitch;
    autosave_toggle_->setChecked(config_->get_screenshot_save_enabled());
    QWidget *screenshot_card = nullptr;
    std::tie(screenshot_card, autosave_lbl_, autosave_desc_) =
        make_setting_row_with_refs(welcome_tr("自动保存截图"), autosave_toggle_, welcome_tr("关闭时截图只复制到剪贴板，不落盘。"));
    path_edit_ = path_field(QDir::toNativeSeparators(config_->get_screenshot_save_path()), true);
    // The same word as the clipboard folder's: to the user both change a folder.
    browse_btn_ = path_button(settings_text("Change"), FluentIcon::Folder);
    connect(browse_btn_, &QPushButton::clicked, this, [this] {
        const QString folder = get_existing_directory(this, welcome_tr("选择保存文件夹"), path_edit_->text().trimmed());
        if (folder.isEmpty()) return;
        path_edit_->setText(QDir::toNativeSeparators(folder));
        path_edit_->setToolTip(path_edit_->text());
    });
    add_under(screenshot_card, path_edit_, browse_btn_);
    // The folder means nothing with saving off, so it greys out rather than mislead.
    const auto sync_save = [this](bool on) {
        path_edit_->setEnabled(on);
        browse_btn_->setEnabled(on);
    };
    connect(autosave_toggle_, &SwitchButton::checkedChanged, this, sync_save);
    sync_save(autosave_toggle_->isChecked());
    layout->addWidget(screenshot_card);

    // ── Clipboard data: moved at once, as in the settings window. ──
    QWidget *clipboard_card = nullptr;
    std::tie(clipboard_card, clipboard_lbl_, clipboard_desc_) =
        make_setting_row_with_refs(welcome_tr("剪贴板数据"), no_control(), welcome_tr("剪贴板历史的数据库和图片都放在这个文件夹。"));
    clipboard_edit_ = path_field(clipboard_folder(config_), false);
    change_btn_ = path_button(settings_text("Change"), FluentIcon::Folder);
    connect(change_btn_, &QPushButton::clicked, this, [this] {
        QPointer<StoragePage> guard(this);
        change_clipboard_data_location(this, [guard](const QString &db) {
            if (!guard) return;
            guard->clipboard_edit_->setText(QDir::toNativeSeparators(QFileInfo(db).absolutePath()));
            guard->clipboard_edit_->setToolTip(guard->clipboard_edit_->text());
        });
    });
    add_under(clipboard_card, clipboard_edit_, change_btn_);
    layout->addWidget(clipboard_card);

    // ── Settings: green mode moves the file at once. ──
    green_toggle_ = new WelcomeToggleSwitch;
    green_toggle_->setChecked(data_location().beside_program);
    green_writable_ = data_location().beside_program || data_folder::folder_writable(data_folder::green_mode_dir());
    green_toggle_->setEnabled(green_writable_);
    QWidget *green_card = nullptr;
    std::tie(green_card, green_lbl_, green_desc_) = make_setting_row_with_refs(settings_text("Green Mode"), green_toggle_, green_description());
    settings_edit_ = path_field(QDir::toNativeSeparators(data_location().settings_file()), false);
    add_under(green_card, settings_edit_, nullptr);
    connect(green_toggle_, &SwitchButton::checkedChanged, this, [this](bool on) {
        const QString error = data_folder::set_green_mode(*config_, on);
        if (!error.isEmpty()) {
            const QSignalBlocker block(green_toggle_);
            green_toggle_->setChecked(!on);
            show_warning_dialog(this, settings_text("Error"), settings_text("Could not move the settings file: %1").replace("%1", error));
            return;
        }
        settings_edit_->setText(QDir::toNativeSeparators(data_location().settings_file()));
        settings_edit_->setToolTip(settings_edit_->text());
    });
    layout->addWidget(green_card);
}

QString StoragePage::green_description() const {
    QString text = settings_text("Keep the settings in the program's folder; copy the whole folder to take them along.");
    if (!green_writable_) text += "\n" + settings_text("The program's folder cannot be written to.");
    return text;
}

void StoragePage::retranslate() {
    title_label->setText(welcome_tr("存储位置"));
    subtitle_label->setText(page_subtitle());
    autosave_lbl_->setText(welcome_tr("自动保存截图"));
    autosave_desc_->setText(welcome_tr("关闭时截图只复制到剪贴板，不落盘。"));
    browse_btn_->setText(settings_text("Change"));
    clipboard_lbl_->setText(welcome_tr("剪贴板数据"));
    clipboard_desc_->setText(welcome_tr("剪贴板历史的数据库和图片都放在这个文件夹。"));
    change_btn_->setText(settings_text("Change"));
    green_lbl_->setText(settings_text("Green Mode"));
    green_desc_->setText(green_description());
}

void StoragePage::save() {
    config_->set_screenshot_save_enabled(autosave_toggle_->isChecked());
    const QString path = path_edit_->text().trimmed();
    if (!path.isEmpty()) {
        config_->set_screenshot_save_path(path);
        QDir().mkpath(path);
    }
}
}
