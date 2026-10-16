#pragma once
#include "core/update_checker.h"
#include "ui/dialogs.h"
#include <QProgressBar>
#include <QPushButton>

namespace jietuba {
class TextEdit;

// Release notes, progress and the "Update and Restart" action of the update controller.
class UpdateDialog : public StandardDialog {
    Q_OBJECT
public:
    explicit UpdateDialog(const ReleaseInfo &release);
    // Lets the dialog close once the worker has accepted the handoff.
    void finish_handoff();
    void set_working(bool working);
    void reject() override;
    TextEdit *notes;
    QLabel *status;
    QProgressBar *progress;
    QPushButton *action;
    QPushButton *cancel_button;
    bool working = false;
    bool committed = false;
signals:
    void update_requested();
    void cancel_requested();
protected:
    void closeEvent(QCloseEvent *event) override;
private:
    bool modal_committed_ = false;
    bool shutdown_ = false;
};
}
