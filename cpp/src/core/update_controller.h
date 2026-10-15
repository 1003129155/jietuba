#pragma once
#include "core/update_checker.h"
#include "core/updater_process.h"
#include <QObject>
#include <QPointer>
#include <utility>

namespace jietuba {
class UpdateDialog;

// What the update needs from the application: whether it may restart now, and the handoff.
class UpdateHost {
public:
    virtual ~UpdateHost() = default;
    // Empty when an update may start; otherwise the reason shown to the user.
    virtual QString update_busy_reason() = 0;
    virtual std::pair<bool, QString> prepare_for_update() = 0;
    virtual void cancel_update_preparation() = 0;
    virtual bool quit_app() = 0;
};

// Downloads the release through the helper, then hands the restart over to its worker.
class UpdateController : public QObject {
    Q_OBJECT
public:
    // runner: a replacement helper process for tests; the controller takes ownership.
    UpdateController(UpdateHost *host, QObject *parent = nullptr, UpdaterProcess *runner = nullptr);
    ~UpdateController() override;
    void present(const ReleaseInfo &release);
    void start();
    void cancel();
    UpdaterProcess *runner;
    QPointer<UpdateDialog> dialog;
    std::optional<ReleaseInfo> release;
    QString downloaded;
    QString mode;
    bool cancelled() const { return cancelled_; }
private:
    QStringList arguments() const;
    void install();
    void on_event(const QString &event, const QString &transaction, const QJsonObject &data);
    void on_error(const QString &reason);
    void on_finished();
    void remove_request();
    void discard_download();
    UpdateHost *host_;
    bool cancelled_ = false;
    bool failed_ = false;
    bool committed_ = false;
    QString request_;
};
}
