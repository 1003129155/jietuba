#include "core/update_checker.h"
#include "core/constants.h"
#include "core/logger.h"
#include "core/update_cache.h"
#include "core/updater_process.h"
#include "main_app.h"
#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QRegularExpression>

namespace jietuba {
std::array<int, 4> comparable_version(const QString &value) {
    static const QRegularExpression pattern(R"((?<!\d)\d+(?:\.\d+){0,3}(?![\d.]))");
    const auto match = pattern.match(value.trimmed());
    if (!match.hasMatch()) throw UpdateCheckError(QString("Invalid release version: '%1'").arg(value).toStdString());
    std::array<int, 4> parts{0, 0, 0, 0};
    const auto pieces = match.captured(0).split('.');
    for (qsizetype i = 0; i < pieces.size() && i < 4; ++i) parts[static_cast<size_t>(i)] = pieces.at(i).toInt();
    return parts;
}

bool is_newer_version(const QString &remote_version, const QString &current_version) {
    return comparable_version(remote_version) > comparable_version(current_version);
}

ReleaseInfo parse_release_payload(const QByteArray &payload) {
    QJsonParseError parse{};
    const auto document = QJsonDocument::fromJson(payload, &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) throw UpdateCheckError("Invalid response from GitHub");
    const QJsonObject data = document.object();
    const auto tag = data.value("tag_name");
    if (!tag.isString() || tag.toString().trimmed().isEmpty()) throw UpdateCheckError("The latest GitHub release has no version tag");
    const QString tag_name = tag.toString().trimmed();
    comparable_version(tag_name);
    const auto title = data.value("name");
    const auto notes = data.value("body");
    ReleaseInfo release;
    release.tag_name = tag_name;
    release.title = title.isString() && !title.toString().trimmed().isEmpty() ? title.toString().trimmed() : tag_name;
    release.notes = notes.isString() ? notes.toString().trimmed() : QString();
    release.url = PROJECT_RELEASES_LATEST_URL;
    return release;
}

ReleaseInfo release_from_check_output(const QByteArray &output, int exit_code) {
    // Every line must parse before any event counts, as in the original.
    QList<QJsonObject> events;
    for (const QByteArray &line : output.split('\n')) {
        const QByteArray text = line.trimmed();
        if (text.isEmpty()) continue;
        QJsonParseError parse{};
        const auto document = QJsonDocument::fromJson(text, &parse);
        if (parse.error != QJsonParseError::NoError || !document.isObject()) throw UpdateCheckError(parse.errorString().toStdString());
        events << document.object();
    }
    for (const QJsonObject &event : events) {
        if (event.value("protocol") != QJsonValue(1)) throw UpdateCheckError("Invalid updater protocol");
        const QString name = event.value("event").toString();
        const QJsonObject data = event.value("data").toObject();
        if (name == "error") throw UpdateCheckError(data.value("message").toString("Update check failed").toStdString());
        if ((name == "available" || name == "up_to_date") && exit_code == 0) {
            for (const char *key : {"tag_name", "title", "notes", "url", "asset_name", "arch"})
                if (!data.value(key).isString()) throw UpdateCheckError(std::string("Missing release field: ") + key);
            if (!data.value("artifact").isObject()) throw UpdateCheckError("Missing release field: artifact");
            return {data.value("tag_name").toString(), data.value("title").toString(), data.value("notes").toString(), data.value("url").toString(),
                    name == "available", data.value("asset_name").toString(), data.value("arch").toString(), data.value("artifact").toObject()};
        }
    }
    throw UpdateCheckError("Updater returned no release");
}

GitHubReleaseChecker::GitHubReleaseChecker(QObject *parent, int timeout_ms) : QObject(parent), timeout_ms_(timeout_ms), timer_(this) {
    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, [this] { stop("Update check timed out"); });
}

GitHubReleaseChecker::~GitHubReleaseChecker() {
    // Closing the settings window ends a running check; its helper copy goes with it.
    if (process_) {
        process_->disconnect(this);
        process_->kill();
        process_->waitForFinished(3000);
    }
    update_cache::remove_helper(helper_);
}

bool GitHubReleaseChecker::check() {
    if (is_checking()) return false;
    try {
        helper_ = helper_path();
    } catch (const std::exception &e) {
        // The original reports a failed check, after its background thread has started.
        const QString reason = QString::fromStdString(e.what());
        QMetaObject::invokeMethod(this, [this, reason] { emit failed(reason); }, Qt::QueuedConnection);
        return true;
    }
    auto *process = new QProcess(this);
    process_ = process;
    connect(process, &QProcess::finished, this, &GitHubReleaseChecker::finish);
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) stop(process->errorString());
    });
    process->start(helper_, {"check", "--install-exe", QDir::toNativeSeparators(QCoreApplication::applicationFilePath()), "--current-version", APP_VERSION,
                             "--variant", app_variant()});
    timer_.start(timeout_ms_);
    return true;
}

void GitHubReleaseChecker::finish(int exit_code, QProcess::ExitStatus status) {
    timer_.stop();
    const QByteArray output = process_ ? process_->readAllStandardOutput() : QByteArray();
    if (process_) process_->deleteLater();
    process_ = nullptr;
    update_cache::remove_helper(helper_);
    helper_.clear();
    try {
        emit release_found(release_from_check_output(output, status == QProcess::NormalExit ? exit_code : -1));
    } catch (const UpdateCheckError &e) {
        emit failed(QString::fromStdString(e.what()));
    }
}

void GitHubReleaseChecker::stop(const QString &error) {
    timer_.stop();
    if (process_) {
        process_->disconnect(this);
        process_->kill();
        process_->waitForFinished(3000);
        process_->deleteLater();
    }
    process_ = nullptr;
    update_cache::remove_helper(helper_);
    helper_.clear();
    emit failed(error);
}
}
