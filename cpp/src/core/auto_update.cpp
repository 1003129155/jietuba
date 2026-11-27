#include "core/auto_update.h"
#include "core/constants.h"
#include "core/logger.h"
#include "core/update_cache.h"
#include "core/update_controller.h"
#include "core/updater_process.h"
#include "main_app.h"
#include "settings/tool_settings.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <memory>

namespace jietuba {
namespace {
// Written into a download's folder before installing it. The worker removes the folder after a
// successful install, so finding the file again means that install did not take.
const QString ATTEMPT_FILE = QStringLiteral("auto_install");

// The helper records the executable as Rust canonicalizes it, with the \\?\ prefix.
QString comparable_path(QString path) {
    path = QDir::toNativeSeparators(path);
    if (path.startsWith(QStringLiteral("\\\\?\\UNC\\"), Qt::CaseInsensitive)) path = "\\\\" + path.mid(8);
    else if (path.startsWith(QStringLiteral("\\\\?\\"))) path = path.mid(4);
    return QDir::cleanPath(QDir::fromNativeSeparators(path)).toLower();
}

QString executable_path(const QString &path) {
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return comparable_path(canonical.isEmpty() ? info.absoluteFilePath() : canonical);
}

bool newer(const QString &version, const QString &than) {
    try {
        return is_newer_version(version, than);
    } catch (const UpdateCheckError &) {
        return false;
    }
}

ReleaseInfo release_from_record(const QJsonObject &data) {
    return {data.value("tag_name").toString(), data.value("title").toString(), data.value("notes").toString(), data.value("url").toString(), true,
            data.value("asset_name").toString(), data.value("arch").toString(), data.value("artifact").toObject(), data.value("published_at").toString()};
}
}

std::optional<PendingUpdate> find_pending_update(const QString &cache_root, const QString &executable, const QString &current_version) {
    static const QRegularExpression id("\\A[0-9a-f]{32}\\z");
    const QString exe = executable_path(executable);
    std::optional<PendingUpdate> best;
    for (const auto &entry : QDir(cache_root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden)) {
        if (!id.match(entry.fileName()).hasMatch() || entry.isSymLink() || entry.isJunction()) continue;
        const QDir dir(entry.absoluteFilePath());
        QFile file(dir.filePath("prepared.json"));
        if (!QFileInfo::exists(dir.filePath("app.exe")) || !file.open(QIODevice::ReadOnly)) continue;
        const QJsonObject prepared = QJsonDocument::fromJson(file.readAll()).object();
        const QJsonObject record = prepared.value("release").toObject();
        const ReleaseInfo release = release_from_record(record);
        if (prepared.value("current_version").toString() != current_version || comparable_path(prepared.value("executable").toString()) != exe) continue;
        if (release.asset_name.isEmpty() || !newer(release.tag_name, current_version)) continue;
        if (best && !newer(release.tag_name, best->release.tag_name)) continue;
        best = PendingUpdate{entry.fileName(), entry.absoluteFilePath(), release};
    }
    return best;
}

AutoUpdater::AutoUpdater(ToolSettingsManager *config, UpdateController *controller, QObject *parent)
    : QObject(parent), timer(this), config_(config), controller_(controller), checker_(new GitHubReleaseChecker(this)) {
    timer.setSingleShot(true);
    connect(&timer, &QTimer::timeout, this, &AutoUpdater::check_now);
    connect(checker_, &GitHubReleaseChecker::release_found, this, &AutoUpdater::on_release);
    connect(checker_, &GitHubReleaseChecker::failed, this, &AutoUpdater::on_check_failed);
    connect(controller_, &UpdateController::background_download_finished, this, &AutoUpdater::on_download_finished);
}

bool AutoUpdater::old_enough(const ReleaseInfo &release, const QDateTime &now) {
    const QDateTime published = QDateTime::fromString(release.published_at, Qt::ISODate);
    return published.isValid() && published.secsTo(now) >= RELEASE_AGE_SECS;
}

bool AutoUpdater::enabled() const { return config_->get_auto_update() && updater_bundled(); }

void AutoUpdater::mark_failed(const QString &version, const QString &reason) {
    config_->set_auto_update_failed_version(version);
    log_warning(T("自动更新到 {version} 没有完成，这个版本不再自动安装: {reason}", {{"version", version}, {"reason", reason}}), "Update");
}

bool AutoUpdater::install_pending(std::function<void()> resume) {
    const QString previous = config_->get_last_run_version();
    if (previous != APP_VERSION) config_->set_last_run_version(APP_VERSION);
    if (!previous.isEmpty() && newer(APP_VERSION, previous)) updated_from_ = previous;
    if (!enabled()) return false;
    std::optional<PendingUpdate> pending;
    try {
        pending = find_pending_update(update_cache::cache_root(), QCoreApplication::applicationFilePath(), APP_VERSION);
    } catch (const std::exception &) {
        return false;
    }
    if (!pending) return false;
    const QString version = pending->release.tag_name;
    const QString attempt = QDir(pending->directory).filePath(ATTEMPT_FILE);
    if (QFileInfo::exists(attempt) || version == config_->get_auto_update_failed_version()) {
        if (QFileInfo::exists(attempt)) mark_failed(version, QStringLiteral("previous attempt"));
        update_cache::remove_directory(pending->directory);
        return false;
    }
    QFile marker(attempt);
    if (!marker.open(QIODevice::WriteOnly)) return false;
    marker.close();
    log_info(T("启动时安装已下载的更新 {version}", {{"version", version}}), "Update");
    auto failed = std::make_shared<QMetaObject::Connection>();
    *failed = connect(controller_, &UpdateController::background_install_failed, this, [this, failed, version, resume](const QString &reason) {
        disconnect(*failed);
        mark_failed(version, reason);
        resume();
    });
    if (controller_->install_in_background(pending->release, pending->transaction)) return true;
    disconnect(*failed);
    mark_failed(version, QStringLiteral("not started"));
    return false;
}

void AutoUpdater::start() {
    started_ = true;
    if (enabled()) timer.start(first_check_ms);
}

void AutoUpdater::set_enabled(bool on) {
    if (!on) {
        timer.stop();
        controller_->cancel_background();
        return;
    }
    if (started_ && !timer.isActive() && !checker_->is_checking() && updater_bundled()) timer.start(first_check_ms);
}

void AutoUpdater::check_now() {
    if (!enabled()) return;
    // A manual update is running; this waits for the next round.
    if (controller_->busy() || checker_->is_checking()) {
        timer.start(retry_ms);
        return;
    }
    checker_->check();
}

void AutoUpdater::on_release(const ReleaseInfo &release) {
    if (!enabled()) return;
    timer.start(interval_ms);
    const bool available = release.available.has_value() ? *release.available : newer(release.tag_name, APP_VERSION);
    if (!available || release.asset_name.isEmpty()) return;
    if (release.tag_name == config_->get_auto_update_failed_version()) return;
    if (!old_enough(release, QDateTime::currentDateTimeUtc())) {
        log_info(T("新版本 {version} 发布未满两天，暂不自动下载", {{"version", release.tag_name}}), "Update");
        return;
    }
    if (controller_->has_download(release)) {
        on_download_finished(true);
        return;
    }
    if (controller_->download_in_background(release)) log_info(T("开始在后台下载更新 {version}", {{"version", release.tag_name}}), "Update");
    else timer.start(retry_ms);
}

void AutoUpdater::on_check_failed(const QString &reason) {
    log_info(T("自动检查更新失败: {reason}", {{"reason", reason}}), "Update");
    if (enabled()) timer.start(retry_ms);
}

void AutoUpdater::on_download_finished(bool ok) {
    if (!ok || !controller_->release) {
        log_info(T("后台下载更新没有完成"), "Update");
        if (enabled()) timer.start(retry_ms);
        return;
    }
    const ReleaseInfo release = *controller_->release;
    log_info(T("更新 {version} 已下载，下次启动时安装", {{"version", release.tag_name}}), "Update");
    if (notified_ == release.tag_name) return;
    notified_ = release.tag_name;
    emit update_ready(release);
}
}
