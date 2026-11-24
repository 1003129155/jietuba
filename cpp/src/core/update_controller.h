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

// Downloads the release through the helper, then hands the restart over to its worker. Automatic
// updates use the same helper without a window: a download kept for the next start, and that
// install at start-up.
class UpdateController : public QObject {
    Q_OBJECT
public:
    // runner: a replacement helper process for tests; the controller takes ownership.
    UpdateController(UpdateHost *host, QObject *parent = nullptr, UpdaterProcess *runner = nullptr);
    ~UpdateController() override;
    // Shows the release; a download of the same package, finished or still running, carries on.
    void present(const ReleaseInfo &release);
    void start();
    void cancel();
    // Downloads without a window and keeps the result; background_download_finished reports it.
    bool download_in_background(const ReleaseInfo &release);
    // Stops a download started by download_in_background that no window has taken over.
    void cancel_background();
    // Installs a download from an earlier run without a window. A failure is reported by
    // background_install_failed, a handoff quits the application.
    bool install_in_background(const ReleaseInfo &release, const QString &transaction);
    bool has_download(const ReleaseInfo &value) const { return !downloaded.isEmpty() && release && release->same_package(value); }
    bool busy() const { return runner->busy(); }
    UpdaterProcess *runner;
    QPointer<UpdateDialog> dialog;
    std::optional<ReleaseInfo> release;
    QString downloaded;
    QString mode;
    bool cancelled() const { return cancelled_; }
signals:
    void background_download_finished(bool ok);
    void background_install_failed(const QString &reason);
private:
    QStringList arguments() const;
    void show_dialog(const ReleaseInfo &value);
    bool begin_download();
    bool install(int timeout_ms = 105000);
    void on_event(const QString &event, const QString &transaction, const QJsonObject &data);
    void on_error(const QString &reason);
    void on_finished();
    void remove_request();
    void discard_download();
    UpdateHost *host_;
    bool cancelled_ = false;
    bool failed_ = false;
    bool committed_ = false;
    // No window is attached; the outcome goes to the background signals.
    bool background_ = false;
    // The download waits for the next start, so closing the window or quitting keeps it.
    bool kept_ = false;
    QString failure_;
    QString request_;
};
}
