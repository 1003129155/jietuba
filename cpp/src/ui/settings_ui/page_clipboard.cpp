#include "clipboard/core/manager.h"
#include "clipboard/ui/windows/clipboard_window.h"
#include "core/logger.h"
#include "core/py_text.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "settings/special_paste.h"
#include "settings/tool_settings.h"
#include "ui/dialogs.h"
#include "ui/fluent_lite/buttons.h"
#include "ui/fluent_lite/cards.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/labels.h"
#include "ui/settings_ui/components.h"
#include "ui/settings_ui/dialog.h"
#include "ui/settings_ui/pages.h"
#include "ui/settings_ui/special_paste_dialog.h"
#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QProgressDialog>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <memory>

namespace jietuba {
namespace {
using clipboard::ClipboardManager;

QString format_size(qint64 value) {
    if (value < 1024) return QString("%1 B").arg(value);
    if (value < 1024LL * 1024) return python_fixed(value / 1024.0, 1) + " KB";
    if (value < 1024LL * 1024 * 1024) return python_fixed(value / (1024.0 * 1024), 1) + " MB";
    return python_fixed(value / (1024.0 * 1024 * 1024), 2) + " GB";
}

QString absolute_path(const QString &path) { return QDir::toNativeSeparators(QDir::cleanPath(QFileInfo(path).absoluteFilePath())); }

QString path_join(const QString &first, const QString &second) { return py_path_join(first, second); }

// Clipboard storage size: the database with its WAL files plus the image folder.
QString calc_clipboard_storage_size() {
    auto &cm = ClipboardManager::instance();
    if (!cm.is_available()) return {};
    qint64 total = 0;
    const QString db_path = cm.get_db_path().value_or(QString());
    if (!db_path.isEmpty()) {
        for (const char *suffix : {"", "-wal", "-shm"}) {
            const QFileInfo info(db_path + suffix);
            if (info.isFile()) total += info.size();
        }
    }
    const QString img_dir = cm.get_images_dir().value_or(QString());
    if (!img_dir.isEmpty() && QFileInfo(img_dir).isDir()) {
        for (const QFileInfo &info : QDir(img_dir).entryInfoList(QDir::Files | QDir::Hidden | QDir::System)) total += info.size();
    }
    return format_size(total);
}

QString get_clipboard_db_path(SettingsDialog *dialog) {
    auto &cm = ClipboardManager::instance();
    if (cm.is_available()) {
        const QString path = cm.get_db_path().value_or(QString());
        return path.isEmpty() ? dialog->tr_text("Unknown") : path;
    }
    return dialog->tr_text("Clipboard module not available");
}

void refresh_clipboard_size_async(SettingsDialog *dialog) {
    // Scans the disk on a worker thread and updates the label only if it still exists.
    QPointer<QLabel> label = dialog->attr<QLabel>("_clipboard_size_label");
    if (!label) return;
    auto result = std::make_shared<QString>();
    QThread *thread = QThread::create([result] { *result = calc_clipboard_storage_size(); });
    QObject::connect(thread, &QThread::finished, label, [label, result] {
        if (label) label->setText(result->isEmpty() ? QString::fromUtf8("—") : *result);
    });
    QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void manage_special_paste(SettingsDialog *dialog) {
    SpecialPasteDialog editor(dialog->clipboard_special_paste_entries.value_or(special_paste::load(dialog->config_manager)), dialog);
    if (exec_dialog(&editor) == QDialog::Accepted) {
        dialog->clipboard_special_paste_entries = editor.entries();
        dialog->update_action_buttons();
    }
}

void set_storage_path_label(SettingsDialog *dialog, const QString &path) {
    if (auto *card = dialog->attr<SettingCard>("_clipboard_storage_card")) card->setContent(path);
}

bool target_has_clipboard_data(const QString &target_dir) {
    const QString db_path = path_join(target_dir, "clipboard.db");
    for (const char *suffix : {"", "-wal", "-shm"})
        if (QFileInfo::exists(db_path + suffix)) return true;
    const QString images_dir = path_join(target_dir, "images");
    if (!QFileInfo(images_dir).isDir()) return false;
    return !QDir(images_dir).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System).isEmpty();
}

// Moving storage releases the backend; anything done in an open clipboard window would
// silently fail meanwhile. close() also unregisters its keys and closes its open menus.
void close_open_clipboard_windows() {
    for (QWidget *widget : QApplication::topLevelWidgets())
        if (auto *window = qobject_cast<clipboard::ClipboardWindow *>(widget); window && window->isVisible()) window->close();
}

void refresh_open_clipboard_windows() {
    for (QWidget *widget : QApplication::topLevelWidgets())
        if (auto *window = qobject_cast<clipboard::ClipboardWindow *>(widget)) window->request_data_refresh("Clipboard storage");
}

bool remove_path_if_exists(const QString &path) {
    const QFileInfo info(path);
    if (info.isDir() && !info.isSymLink()) return QDir(path).removeRecursively();
    if (info.exists() || info.isSymLink()) return QFile::remove(path);
    return true;
}

struct MoveTexts {
    QString finalizing, complete, no_files, moving;
};

// Copies the database and images into a staging folder under the target, swaps them in
// (restoring what was there on failure) and removes the source. Runs on a worker thread;
// progress(percent, text) is called from it.
class ClipboardStorageMover {
public:
    ClipboardStorageMover(const QString &source_db_path, const QString &target_dir, MoveTexts texts)
        : source_db_path_(absolute_path(source_db_path)), target_dir_(absolute_path(target_dir)), texts_(std::move(texts)) {}

    std::function<void(int, const QString &)> progress;
    // (new database path, cleanup error) on success; the message on failure.
    std::function<void(const QString &, const QString &)> completed;
    std::function<void(const QString &)> failed;

    void run() {
        const QString staging_dir = path_join(target_dir_, ".clipboard_migration_tmp");
        const QString target_db_path = path_join(target_dir_, "clipboard.db");
        try {
            const auto payload = collect_payload();
            qint64 total_bytes = 0;
            for (const auto &entry : payload) total_bytes += entry.size;
            qint64 copied = 0;
            if (QFileInfo::exists(staging_dir)) QDir(staging_dir).removeRecursively();
            if (!QDir().mkpath(staging_dir)) throw std::runtime_error(("Cannot create " + staging_dir).toStdString());
            for (const auto &entry : payload) copied += copy_file(entry.source, path_join(staging_dir, entry.relative_target), total_bytes, copied, entry.size);

            progress(99, texts_.finalizing);
            replace_target_from_staging(staging_dir);
            QString cleanup_error;
            try {
                remove_source_payload();
            } catch (const std::exception &e) {
                cleanup_error = QString::fromUtf8(e.what());
            }
            progress(100, texts_.complete);
            completed(target_db_path, cleanup_error);
        } catch (const std::exception &e) {
            if (QFileInfo::exists(staging_dir)) QDir(staging_dir).removeRecursively();
            failed(QString::fromUtf8(e.what()));
        }
    }

private:
    struct Entry {
        QString source;
        QString relative_target;
        qint64 size;
    };

    QList<Entry> collect_payload() const {
        QList<Entry> payload;
        for (const char *suffix : {"", "-wal", "-shm"}) {
            const QFileInfo info(source_db_path_ + suffix);
            if (info.isFile()) payload.append({info.filePath(), QString("clipboard.db") + suffix, info.size()});
        }
        const QString images_dir = path_join(py_dirname(source_db_path_), "images");
        if (QFileInfo(images_dir).isDir()) {
            QDirIterator it(images_dir, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                const QFileInfo info(it.next());
                const QString rel = QDir::toNativeSeparators(QDir(images_dir).relativeFilePath(info.filePath()));
                payload.append({QDir::toNativeSeparators(info.filePath()), path_join("images", rel), info.size()});
            }
        }
        if (payload.isEmpty()) throw std::runtime_error(texts_.no_files.toStdString());
        return payload;
    }

    qint64 copy_file(const QString &source, const QString &destination, qint64 total_bytes, qint64 copied_before, qint64 file_size) {
        QDir().mkpath(py_dirname(destination));
        QFile src(source), dst(destination);
        if (!src.open(QIODevice::ReadOnly)) throw std::runtime_error((src.errorString() + ": " + source).toStdString());
        if (!dst.open(QIODevice::WriteOnly | QIODevice::Truncate)) throw std::runtime_error((dst.errorString() + ": " + destination).toStdString());
        qint64 copied_file = 0;
        while (true) {
            const QByteArray chunk = src.read(1024 * 1024);
            if (chunk.isEmpty()) break;
            if (dst.write(chunk) != chunk.size()) throw std::runtime_error((dst.errorString() + ": " + destination).toStdString());
            copied_file += chunk.size();
            const qint64 done = copied_before + copied_file;
            const int percent = total_bytes ? static_cast<int>(done * 98 / total_bytes) : 98;
            progress(std::max(0, std::min(percent, 98)), QString(texts_.moving).replace("{done}", format_size(done)).replace("{total}", format_size(total_bytes)));
        }
        dst.close();
        // shutil.copystat: keep the times
        QFile stamped(destination);
        if (stamped.open(QIODevice::ReadWrite)) {
            const QFileInfo info(source);
            stamped.setFileTime(info.lastModified(), QFileDevice::FileModificationTime);
            stamped.setFileTime(info.lastRead(), QFileDevice::FileAccessTime);
        }
        return file_size;
    }

    void replace_target_from_staging(const QString &staging_dir) {
        QDir().mkpath(target_dir_);
        const QString backup_dir = path_join(target_dir_, ".clipboard_migration_backup");
        const QStringList target_names{"clipboard.db", "clipboard.db-wal", "clipboard.db-shm", "images"};
        const QStringList staging_names = QDir(staging_dir).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        if (QFileInfo::exists(backup_dir)) QDir(backup_dir).removeRecursively();
        QDir().mkpath(backup_dir);
        auto cleanup = [&] {
            QDir(staging_dir).removeRecursively();
            QDir(backup_dir).removeRecursively();
        };
        try {
            for (const auto &name : target_names) {
                const QString path = path_join(target_dir_, name);
                if (QFileInfo::exists(path) && !QDir().rename(path, path_join(backup_dir, name)))
                    throw std::runtime_error(("Cannot move " + path).toStdString());
            }
            try {
                for (const auto &name : staging_names)
                    if (!QDir().rename(path_join(staging_dir, name), path_join(target_dir_, name)))
                        throw std::runtime_error(("Cannot move " + path_join(staging_dir, name)).toStdString());
            } catch (...) {
                for (const auto &name : staging_names) remove_path_if_exists(path_join(target_dir_, name));
                for (const auto &name : QDir(backup_dir).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System))
                    QDir().rename(path_join(backup_dir, name), path_join(target_dir_, name));
                throw;
            }
        } catch (...) {
            cleanup();
            throw;
        }
        cleanup();
    }

    void remove_source_payload() {
        const QString source_dir = py_dirname(source_db_path_);
        if (py_normcase(target_dir_) == py_normcase(absolute_path(source_dir))) return;
        for (const char *suffix : {"", "-wal", "-shm"}) {
            const QString path = source_db_path_ + suffix;
            if (QFileInfo::exists(path) && !QFile::remove(path)) throw std::runtime_error(("Cannot remove " + path).toStdString());
        }
        const QString images_dir = path_join(source_dir, "images");
        if (QFileInfo(images_dir).isDir() && !QDir(images_dir).removeRecursively()) throw std::runtime_error(("Cannot remove " + images_dir).toStdString());
    }

    QString source_db_path_;
    QString target_dir_;
    MoveTexts texts_;
};

// The storage is reopened even when the settings window is gone, so the clipboard is never left released.
void on_clipboard_move_completed(SettingsDialog *dialog, QProgressDialog *progress, const QString &new_db_path, const QString &cleanup_error) {
    if (progress) {
        progress->setValue(100);
        progress->close();
        progress->deleteLater();
    }
    get_tool_settings_manager()->set_clipboard_db_path(new_db_path);
    const bool reopened = ClipboardManager::instance().reset_storage(new_db_path);
    if (reopened) refresh_open_clipboard_windows();
    if (!dialog) return;
    dialog->clipboard_move_thread = nullptr;
    if (reopened) {
        set_storage_path_label(dialog, new_db_path);
        refresh_clipboard_size_async(dialog);
        if (!cleanup_error.isEmpty())
            show_warning_dialog(dialog, dialog->tr_text("Success"),
                                dialog
                                    ->tr_text("Clipboard data location changed successfully, but the old files are still in use and could not be deleted. You "
                                              "can remove them after restarting the app.\n{error}")
                                    .replace("{error}", cleanup_error));
    } else {
        show_warning_dialog(dialog, dialog->tr_text("Error"), dialog->tr_text("Clipboard data moved, but reopening the database failed. Please restart the app."));
    }
}

void on_clipboard_move_failed(SettingsDialog *dialog, QProgressDialog *progress, const QString &old_db_path, const QString &message) {
    if (progress) {
        progress->close();
        progress->deleteLater();
    }
    ClipboardManager::instance().reset_storage(old_db_path);
    if (!dialog) return;
    dialog->clipboard_move_thread = nullptr;
    show_warning_dialog(dialog, dialog->tr_text("Error"), message);
}

void change_clipboard_data_location(SettingsDialog *dialog) {
    auto &cm = ClipboardManager::instance();
    if (!cm.is_available()) {
        show_warning_dialog(dialog, dialog->tr_text("Error"), dialog->tr_text("Clipboard module not available"));
        return;
    }
    const QString current_db_path = cm.get_db_path().value_or(QString());
    if (current_db_path.isEmpty()) {
        show_warning_dialog(dialog, dialog->tr_text("Error"), dialog->tr_text("Current clipboard database path is unavailable."));
        return;
    }
    const QString current_dir = py_dirname(current_db_path);
    const QString target_dir = get_existing_directory(dialog, dialog->tr_text("Choose Clipboard Data Folder"), current_dir);
    if (target_dir.isEmpty()) return;

    const QString current_dir_norm = py_normcase(absolute_path(current_dir));
    const QString target_dir_norm = py_normcase(absolute_path(target_dir));
    if (current_dir_norm == target_dir_norm) {
        show_info_dialog(dialog, dialog->tr_text("Info"), dialog->tr_text("The selected folder is already the current location."));
        return;
    }
    const QString images_dir = path_join(current_dir, "images");
    if (QFileInfo(images_dir).isDir()) {
        const QString images_dir_norm = py_normcase(absolute_path(images_dir));
        // os.path.commonpath: the target is the images folder or inside it
        if (target_dir_norm == images_dir_norm || target_dir_norm.startsWith(images_dir_norm + "\\")) {
            show_warning_dialog(dialog, dialog->tr_text("Error"), dialog->tr_text("Please choose a folder outside the current clipboard images folder."));
            return;
        }
    }
    if (target_has_clipboard_data(target_dir) &&
        !show_confirm_dialog(dialog, dialog->tr_text("Replace Existing Data"),
                             dialog->tr_text("The selected folder already contains clipboard data. Replace it with the current data?")))
        return;

    close_open_clipboard_windows();
    if (!cm.release_storage()) {
        show_warning_dialog(dialog, dialog->tr_text("Error"), dialog->tr_text("Failed to close the current clipboard database."));
        return;
    }

    auto *progress = new QProgressDialog(dialog->tr_text("Preparing migration..."), "", 0, 100, dialog);
    progress->setWindowTitle(dialog->tr_text("Moving Clipboard Data"));
    progress->setCancelButton(nullptr);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);
    progress->show();

    auto mover = std::make_shared<ClipboardStorageMover>(
        current_db_path, target_dir,
        MoveTexts{dialog->tr_text("Finalizing migration..."), dialog->tr_text("Migration complete."), dialog->tr_text("No clipboard data files were found to move."),
                  dialog->tr_text("Moving clipboard data... {done} / {total}")});
    QPointer<QProgressDialog> progress_guard(progress);
    QPointer<SettingsDialog> dialog_guard(dialog);
    // The worker's calls are queued to the application object and run on the GUI thread.
    mover->progress = [progress_guard](int value, const QString &text) {
        QMetaObject::invokeMethod(qApp, [progress_guard, value, text] {
            if (!progress_guard) return;
            progress_guard->setValue(value);
            progress_guard->setLabelText(text);
        }, Qt::QueuedConnection);
    };
    mover->completed = [dialog_guard, progress_guard](const QString &new_path, const QString &cleanup_error) {
        QMetaObject::invokeMethod(qApp, [dialog_guard, progress_guard, new_path, cleanup_error] {
            on_clipboard_move_completed(dialog_guard, progress_guard, new_path, cleanup_error);
        }, Qt::QueuedConnection);
    };
    mover->failed = [dialog_guard, progress_guard, current_db_path](const QString &message) {
        QMetaObject::invokeMethod(qApp, [dialog_guard, progress_guard, current_db_path, message] {
            on_clipboard_move_failed(dialog_guard, progress_guard, current_db_path, message);
        }, Qt::QueuedConnection);
    };
    // Not parented to the window: destroying a running thread with it would abort the process.
    QThread *worker = QThread::create([mover] { mover->run(); });
    QObject::connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    dialog->clipboard_move_thread = worker;
    worker->start();
}

void open_clipboard_data_folder(SettingsDialog *dialog, const QString &path) {
    const QString folder = QFileInfo(path).isFile() ? py_dirname(path) : path;
    if (QFileInfo::exists(folder)) {
        // Opened from the event loop rather than inside the button's mouse release.
        const QString normalized = py_normpath(folder);
        QTimer::singleShot(0, dialog, [normalized] { QDesktopServices::openUrl(QUrl::fromLocalFile(normalized)); });
    } else {
        show_warning_dialog(dialog, dialog->tr_text("Warning"), dialog->tr_text("Folder does not exist"));
    }
}

void clear_clipboard_history(SettingsDialog *dialog) {
    const auto [confirmed, delete_grouped] = show_confirm_checkbox_dialog(
        dialog, dialog->tr_text("Confirm Clear"), dialog->tr_text("Are you sure you want to clear all clipboard history?\nThis action cannot be undone."),
        dialog->tr_text("Also delete saved content in groups"));
    if (!confirmed) return;
    auto &cm = ClipboardManager::instance();
    if (cm.is_available() && cm.clear_history(!delete_grouped)) {
        show_info_dialog(dialog, dialog->tr_text("Success"), dialog->tr_text("Clipboard history cleared successfully"));
        dialog->refresh_clipboard_size(800);
    } else {
        show_warning_dialog(dialog, dialog->tr_text("Error"), dialog->tr_text("Failed to clear clipboard history"));
    }
}
}

QWidget *create_clipboard_page(SettingsDialog *dialog) {
    auto *scroll = page_scroll_area(dialog);
    scroll->setWidgetResizable(true);
    scroll->setStyleSheet("QScrollArea { border: none; background: transparent; }");

    auto *view = new QWidget;
    set_own_style(view, "background: transparent;");
    auto *layout = new QVBoxLayout(view);
    layout->setContentsMargins(0, 0, dialog_scaled(10), 0);
    layout->setSpacing(dialog_scaled(20));

    // ════ Basic settings ════
    auto *grp_basic = new SettingsCardGroup(dialog->tr_text("Basic Settings"), view, "clipboard");
    auto *enabled_card = new SwitchSettingCard(FluentIcon::Paste, dialog->tr_text("Enable Clipboard Manager"), dialog->tr_text("Monitor and manage clipboard history"), grp_basic);
    enabled_card->setChecked(dialog->config_manager->get_clipboard_enabled());
    dialog->set_attr("clipboard_enabled_toggle", enabled_card);
    grp_basic->addSettingCard(enabled_card);
    layout->addWidget(grp_basic);

    // ════ History ════
    auto *grp_history = new SettingsCardGroup(dialog->tr_text("History"), view, "clipboard");
    auto *limit_card = new SettingCard(FluentIcon::History, dialog->tr_text("History Limit"), dialog->tr_text("Maximum number of items to keep (0 = unlimited)"), grp_history);
    auto *limit_spin = new SpinBox(limit_card);
    limit_spin->setRange(0, 10000);
    limit_spin->setValue(static_cast<int>(dialog->config_manager->get_clipboard_history_limit()));
    dialog->set_attr("clipboard_history_limit_spin", limit_spin);
    limit_card->addControl(limit_spin);
    grp_history->addSettingCard(limit_card);
    layout->addWidget(grp_history);

    auto *grp_special_paste = new SettingsCardGroup(dialog->tr_text("Special Paste"), view, "clipboard");
    auto *special_paste_card = new SettingCard(FluentIcon::Paste, dialog->tr_text("Special Paste Menu"),
                                               dialog->tr_text("Choose which paste options appear and arrange their order."), grp_special_paste);
    dialog->clipboard_special_paste_entries = special_paste::load(dialog->config_manager);
    auto *manage_btn = new PushButton(dialog->tr_text("Manage"), special_paste_card);
    dialog->set_attr("special_paste_manage_btn", manage_btn);
    QObject::connect(manage_btn, &QPushButton::clicked, dialog, [dialog] { manage_special_paste(dialog); });
    special_paste_card->addControl(manage_btn);
    grp_special_paste->addSettingCard(special_paste_card);
    layout->addWidget(grp_special_paste);

    // ════ Data ════
    auto *grp_data = new SettingsCardGroup(dialog->tr_text("Data"), view, "clipboard");
    auto *storage_card = new SettingCard(FluentIcon::Folder, dialog->tr_text("Data Storage Location"), get_clipboard_db_path(dialog), grp_data);
    dialog->set_attr("_clipboard_storage_card", storage_card);
    auto *change_folder_btn = new PushButton(dialog->tr_text("Change"), storage_card);
    QObject::connect(change_folder_btn, &QPushButton::clicked, dialog, [dialog] { change_clipboard_data_location(dialog); });
    auto *open_folder_btn = new PushButton(dialog->tr_text("Open"), storage_card);
    QObject::connect(open_folder_btn, &QPushButton::clicked, dialog, [dialog] { open_clipboard_data_folder(dialog, get_clipboard_db_path(dialog)); });
    storage_card->addControl(change_folder_btn);
    storage_card->addControl(open_folder_btn);
    grp_data->addSettingCard(storage_card);

    auto *cleanup_card = new SettingCard(FluentIcon::Delete, dialog->tr_text("Clear Clipboard History"), dialog->tr_text("Delete all clipboard history records"), grp_data);
    // The size needs a disk scan, so it refreshes on demand; the button and result sit left of the control column so the buttons line up with other cards.
    auto *size_label = new CaptionLabel(dialog->tr_text("← Get storage size"), cleanup_card);
    // CaptionLabel wraps and may shrink to zero width (for card descriptions); this one-line result sizes to its content.
    size_label->setWordWrap(false);
    size_label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    dialog->set_attr("_clipboard_size_label", size_label);
    auto *refresh_btn = new TransparentToolButton(FluentIcon::Sync, cleanup_card);
    refresh_btn->setBaseMetrics(48, 32);
    QObject::connect(refresh_btn, &QToolButton::clicked, dialog, [dialog] { refresh_clipboard_size_async(dialog); });
    dialog->set_attr("_clipboard_refresh_btn", refresh_btn);
    const int size_index = cleanup_card->hBoxLayout->count() - 1;
    cleanup_card->hBoxLayout->insertWidget(size_index, refresh_btn);
    cleanup_card->hBoxLayout->insertWidget(size_index + 1, size_label);
    auto *clear_btn = new PrimaryPushButton(dialog->tr_text("Clear History"), cleanup_card);
    QObject::connect(clear_btn, &QPushButton::clicked, dialog, [dialog] { clear_clipboard_history(dialog); });
    cleanup_card->addControl(clear_btn);
    grp_data->addSettingCard(cleanup_card);
    layout->addWidget(grp_data);

    auto *hint = new CaptionLabel(dialog->tr_text("💡 Hint: Set clipboard hotkey in Shortcuts settings."), view);
    hint->setStyleSheet("padding: 5px;");
    layout->addWidget(hint);

    layout->addStretch();
    scroll->setWidget(view);
    return scroll;
}

QString clipboard_storage_size_text() { return calc_clipboard_storage_size(); }
}
