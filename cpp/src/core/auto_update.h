#pragma once
#include "core/update_checker.h"
#include <QDateTime>
#include <QObject>
#include <QTimer>
#include <functional>
#include <optional>

namespace jietuba {
class ToolSettingsManager;
class UpdateController;

// A download from an earlier run that this executable can install.
struct PendingUpdate {
    QString transaction;
    QString directory;
    ReleaseInfo release;
};
// The newest download in the updater cache staged for this executable at this version.
std::optional<PendingUpdate> find_pending_update(const QString &cache_root, const QString &executable, const QString &current_version);

// Automatic updates while the auto_update setting is on: a check a minute after start-up and then
// daily, a background download once a release has been public for two days, and the install at the
// next start. A manual check offers every release at once.
class AutoUpdater : public QObject {
    Q_OBJECT
public:
    static constexpr qint64 RELEASE_AGE_SECS = 2 * 24 * 3600;
    AutoUpdater(ToolSettingsManager *config, UpdateController *controller, QObject *parent = nullptr);
    // Whether the release has been public long enough to install without asking.
    static bool old_enough(const ReleaseInfo &release, const QDateTime &now);
    // Start-up, before anything else runs: installs a download from the last run. True while the
    // install runs; when it fails, resume continues start-up. A handoff quits the application.
    bool install_pending(std::function<void()> resume);
    // The schedule starts once start-up is done.
    void start();
    // Follows the setting: on, a check follows shortly; off, the schedule and a download stop.
    void set_enabled(bool enabled);
    void check_now();
    // The version the last start ran, when this one is newer; empty otherwise.
    QString updated_from() const { return updated_from_; }
    bool checking() const { return checker_->is_checking(); }
    int first_check_ms = 60 * 1000;
    int interval_ms = 24 * 3600 * 1000;
    // After a failed check or download.
    int retry_ms = 2 * 3600 * 1000;
    QTimer timer;
signals:
    // A release is downloaded and installs at the next start.
    void update_ready(const ReleaseInfo &release);
private:
    bool enabled() const;
    void on_release(const ReleaseInfo &release);
    void on_check_failed(const QString &reason);
    void on_download_finished(bool ok);
    void mark_failed(const QString &version, const QString &reason);
    ToolSettingsManager *config_;
    UpdateController *controller_;
    GitHubReleaseChecker *checker_;
    bool started_ = false;
    QString notified_;
    QString updated_from_;
};
}
