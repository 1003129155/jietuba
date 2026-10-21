#pragma once
#include "ui/dialogs.h"
#include "ui/fluent_lite/buttons.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/save_image_dialog.h"
#include <QApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QPushButton>
#include <QTimer>

// Answers the next SaveImageDialog: saves to path (folder, name and the format from its
// extension), cancels when path is empty, or switches to the classic dialog and confirms.
class SaveDialogAnswerer : public QObject {
public:
    enum Action { SAVE, CANCEL, CLASSIC };
    explicit SaveDialogAnswerer(QString path) : SaveDialogAnswerer(path.isEmpty() ? CANCEL : SAVE, path) {}
    SaveDialogAnswerer(Action action, QString path = {}) : action_(action), path_(std::move(path)) {
        timer_.setInterval(20);
        QObject::connect(&timer_, &QTimer::timeout, this, [this] { poll(); });
        timer_.start();
    }
    bool answered = false;

private:
    void poll() {
        auto *dialog = qobject_cast<jietuba::SaveImageDialog *>(QApplication::activeModalWidget());
        if (!dialog || !dialog->isVisible()) return;
        timer_.stop();
        answered = true;
        if (action_ == CANCEL) {
            dialog->reject();
        } else if (action_ == CLASSIC) {
            QTimer::singleShot(0, [] { confirm(); });
            emit dialog->classic_link->linkActivated("classic");
        } else {
            const QFileInfo info(path_);
            dialog->directory_edit->setText(QDir::toNativeSeparators(info.absolutePath()));
            dialog->name_edit->setText(info.completeBaseName());
            dialog->format_combo->setCurrentIndex(dialog->format_combo->findData(info.suffix().toUpper()));
            dialog->save_button->click();
        }
    }
    // Clicks the accepting button of the confirmation once it is open.
    static void confirm() {
        auto *box = qobject_cast<jietuba::StandardDialog *>(QApplication::activeModalWidget());
        if (!box) {
            QTimer::singleShot(20, [] { confirm(); });
            return;
        }
        for (auto *button : box->button_box->buttons())
            if (box->button_box->buttonRole(button) == QDialogButtonBox::YesRole) button->click();
    }
    Action action_;
    QString path_;
    QTimer timer_;
};
