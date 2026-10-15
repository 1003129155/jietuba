#pragma once
#include "clipboard/controllers/paste_keystroke.h"
#include "clipboard/core/manager.h"
#include "clipboard/ui/widgets/preview_popup.h"
#include "clipboard/ui/windows/clipboard_window.h"
#include "settings/tool_settings.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPointer>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>

// Shared setup of the clipboard tests: settings, logs, window geometry and the database
// live in a throwaway folder; each test can open an empty database; pastes are recorded
// instead of pressing Ctrl+V into whatever window is in front.
namespace clipboard_test {
using namespace jietuba;
using namespace jietuba::clipboard;

class Environment {
public:
    QTemporaryDir dir;
    ToolSettingsManager *config = nullptr;
    QList<Hwnd> pastes;

    bool init(QObject *owner) {
        if (!dir.isValid()) return false;
        config = get_tool_settings_manager(new QSettings(dir.filePath("settings.ini"), QSettings::IniFormat, owner));
        config->set_log_dir(dir.filePath("logs"));
        // Applying settings checks global hotkeys against the system, where another program may hold the defaults.
        config->set_hotkey("");
        config->set_clipboard_hotkey("");
        ClipboardWindow::set_geometry_file_for_test(dir.filePath("window.ini"));
        set_paste_to_target_for_test([this](Hwnd hwnd) { pastes.append(hwnd); });
        return fresh_database();
    }

    // An empty database at a new path; ids start from 1 again.
    bool fresh_database() {
        const QString path = QDir::toNativeSeparators(dir.filePath(QString("db%1/clipboard.db").arg(++db_count_)));
        config->set_clipboard_db_path(path);
        pastes.clear();
        return ClipboardManager::instance().reset_storage(path);
    }

    QString db_folder() const { return dir.filePath(QString("db%1").arg(db_count_)); }

private:
    int db_count_ = 0;
};

// Answers modal dialogs while alive: clicks their yes / accept or no / reject button and
// keeps the label text of each one answered.
class ModalAnswerer : public QObject {
public:
    explicit ModalAnswerer(bool accept, bool check_box = false) : accept_(accept), check_box_(check_box) {
        timer_.setInterval(10);
        QObject::connect(&timer_, &QTimer::timeout, this, [this] { poll(); });
        timer_.start();
    }
    QStringList texts;
    int answered() const { return static_cast<int>(texts.size()); }

private:
    void poll() {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog || dialog == last_ || qobject_cast<QFileDialog *>(dialog) || qobject_cast<QProgressDialog *>(dialog)) return;
        QString text;
        for (auto *label : dialog->findChildren<QLabel *>()) text += label->text() + "\n";
        if (check_box_)
            for (auto *box : dialog->findChildren<QCheckBox *>()) box->setChecked(true);
        QPushButton *target = nullptr;
        if (auto *buttons = dialog->findChild<QDialogButtonBox *>()) {
            for (auto *button : buttons->buttons()) {
                const auto role = buttons->buttonRole(button);
                const bool yes = role == QDialogButtonBox::YesRole || role == QDialogButtonBox::AcceptRole;
                const bool no = role == QDialogButtonBox::NoRole || role == QDialogButtonBox::RejectRole;
                if ((accept_ && yes) || (!accept_ && no)) target = qobject_cast<QPushButton *>(button);
            }
        }
        last_ = dialog;
        texts << text;
        if (target) target->click();
        else if (accept_) dialog->accept();
        else dialog->reject();
    }
    bool accept_, check_box_;
    QTimer timer_;
    QPointer<QDialog> last_;
};

// Answers the next file dialog with this path (a folder for folder pickers); empty cancels.
class FileDialogAnswerer : public QObject {
public:
    explicit FileDialogAnswerer(QString path) : path_(std::move(path)) {
        timer_.setInterval(20);
        QObject::connect(&timer_, &QTimer::timeout, this, [this] { poll(); });
        timer_.start();
    }
    bool answered = false;

private:
    void poll() {
        auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
        auto *name = dialog ? dialog->findChild<QLineEdit *>("fileNameEdit") : nullptr;
        if (!dialog || !dialog->isVisible() || !name) return;
        timer_.stop();
        answered = true;
        if (path_.isEmpty()) {
            dialog->reject();
            return;
        }
        const QFileInfo info(path_);
        if (dialog->fileMode() == QFileDialog::Directory) {
            dialog->setDirectory(path_);
            name->clear();
        } else {
            // Typed like a person would: selectFile is reset when the dialog finishes showing.
            dialog->setDirectory(info.absolutePath());
            name->setText(info.fileName());
        }
        static_cast<QDialog *>(dialog)->accept();
    }
    QString path_;
    QTimer timer_;
};

// Waits for the next popup menu, records the texts of all its entries (submenus included)
// and triggers the one with this text after closing the menu; an empty text only closes it.
class MenuPicker : public QObject {
public:
    explicit MenuPicker(QString text = {}) : text_(std::move(text)) {
        timer_.setInterval(10);
        QObject::connect(&timer_, &QTimer::timeout, this, [this] { poll(); });
        timer_.start();
    }
    QStringList seen;
    bool shown = false;
    bool picked = false;
    QAction *found(const QString &text) const { return actions_.value(text); }

private:
    void poll() {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (!menu || shown) return;
        shown = true;
        timer_.stop();
        for (auto *action : menu->findChildren<QAction *>()) {
            if (action->text().isEmpty()) continue;
            seen << action->text();
            actions_.insert(action->text(), action);
        }
        QPointer<QAction> target = actions_.value(text_);
        menu->close();
        if (target && !text_.isEmpty() && target->isEnabled()) {
            picked = true;
            target->trigger();
        }
    }
    QString text_;
    QTimer timer_;
    QHash<QString, QPointer<QAction>> actions_;
};

inline QList<qint64> ids_of(const QList<ClipboardItem> &items) {
    QList<qint64> ids;
    for (const auto &item : items) ids << item.id;
    return ids;
}

inline QStringList contents_of(const QList<ClipboardItem> &items) {
    QStringList contents;
    for (const auto &item : items) contents << item.content;
    return contents;
}

// Adds text items oldest first; returns their ids in the same order.
inline QList<qint64> add_texts(const QStringList &contents) {
    QList<qint64> ids;
    for (const auto &content : contents) ids << ClipboardManager::instance().add_item(content).value_or(-1);
    return ids;
}

inline QString file_payload(const QStringList &paths) {
    QStringList quoted;
    for (QString path : paths) quoted << "\"" + path.replace("\\", "\\\\") + "\"";
    return "{\"files\": [" + quoted.join(", ") + "]}";
}
}
