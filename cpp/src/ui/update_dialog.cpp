#include "ui/update_dialog.h"
#include "core/resource_manager.h"
#include "ui/fluent_lite/inputs.h"
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QVBoxLayout>

namespace jietuba {
namespace {
QString tr_dialog(const char *text) { return QCoreApplication::translate("UpdateDialog", text); }
}

UpdateDialog::UpdateDialog(const ReleaseInfo &release) : StandardDialog(nullptr, "", "") {
    setWindowModality(Qt::NonModal);
    setWindowTitle(tr_dialog("Update Available"));
    setWindowIcon(ResourceManager::get_icon_by_name(QString::fromUtf8("托盘.svg")));
    resize(660, 480);
    auto *box = static_cast<QVBoxLayout *>(layout());
    box->setContentsMargins(18, 14, 18, 14);
    box->setSpacing(10);
    notes = new TextEdit(this);
    notes->setReadOnly(true);
    notes->setPlainText(release.tag_name + "\n\n" + (release.notes.isEmpty() ? tr_dialog("No release notes provided.") : release.notes));
    box->insertWidget(0, notes, 1);
    status = label;
    status->setText(tr_dialog("The application will restart after the download finishes."));
    progress = new QProgressBar(this);
    progress->setRange(0, 100);
    box->insertWidget(box->count() - 1, progress);
    action = button_box->addButton(tr_dialog("Update and Restart"), QDialogButtonBox::ActionRole);
    connect(action, &QPushButton::clicked, this, &UpdateDialog::update_requested);
    cancel_button = button_box->addButton(tr_dialog("Cancel"), QDialogButtonBox::RejectRole);
    connect(cancel_button, &QPushButton::clicked, this, &QWidget::close);
}

void UpdateDialog::finish_handoff() {
    shutdown_ = true;
    close();
}

void UpdateDialog::set_working(bool value) {
    working = value;
    action->setEnabled(!value);
    cancel_button->setEnabled(!committed);
    if (modal_committed_ != committed) {
        // Block new tasks after handoff; settings have already been saved or discarded.
        const bool visible = isVisible();
        if (visible) hide();
        setWindowModality(committed ? Qt::ApplicationModal : Qt::NonModal);
        modal_committed_ = committed;
        if (visible) show();
    }
}

void UpdateDialog::closeEvent(QCloseEvent *event) {
    if (shutdown_) {
        event->accept();
        return;
    }
    if (committed) {
        event->ignore();
        return;
    }
    emit cancel_requested();
    event->accept();
}

void UpdateDialog::reject() { close(); }
}
