#include "core/updater_process.h"
#include "core/logger.h"
#include "core/update_cache.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QUuid>
#include <stdexcept>

namespace jietuba {
namespace {
constexpr int MAX_LINE = 65536;
// Built from the reference updater crate and embedded by CMake when JIETUBA_UPDATES is on.
const QString BUNDLED_HELPER = QStringLiteral(":/updater/jietuba_updater.exe");

QString &helper_source_override() {
    static QString path;
    return path;
}

QString tr_process(const char *text) { return QCoreApplication::translate("UpdaterProcess", text); }
}

bool updater_bundled() { return !helper_source_override().isEmpty() || QFileInfo::exists(BUNDLED_HELPER); }

void set_helper_source_for_test(const QString &path) { helper_source_override() = path; }

QString helper_path() {
    const QString source = helper_source_override().isEmpty() ? BUNDLED_HELPER : helper_source_override();
    if (!QFileInfo::exists(source)) throw std::runtime_error("This build has no updater helper; configure with -DJIETUBA_UPDATES=ON");
    const QString root = QDir(update_cache::cache_subdirectory("helpers")).filePath(QUuid::createUuid().toString(QUuid::Id128));
    if (!QDir().mkdir(root)) throw std::runtime_error("Cannot create " + root.toStdString());
    const QString target = QDir(root).filePath("jietuba_updater.exe");
    // A copy out of the resource is read-only, and cache cleanup must be able to delete it.
    if (!QFile::copy(source, target) || !QFile::setPermissions(target, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) {
        update_cache::remove_directory(root);
        throw std::runtime_error("Cannot copy the updater helper to " + target.toStdString());
    }
    return target;
}

QString app_variant() { return "lite"; }

UpdaterProcess::UpdaterProcess(QObject *parent) : QObject(parent), process_(this), timer_(this) {
    connect(&process_, &QProcess::readyReadStandardOutput, this, &UpdaterProcess::read);
    connect(&process_, &QProcess::readyReadStandardError, this, &UpdaterProcess::read_stderr);
    connect(&process_, &QProcess::finished, this, &UpdaterProcess::on_finished);
    connect(&process_, &QProcess::errorOccurred, this, &UpdaterProcess::on_process_error);
    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, &UpdaterProcess::on_timeout);
}

bool UpdaterProcess::start(const QString &command, const QStringList &arguments, int timeout_ms) {
    if (busy_) return false;
    buffer_.clear();
    stderr_.clear();
    terminal_ = error_ = handed_off_ = false;
    command_ = command;
    downloaded_ = false;
    const qsizetype given = arguments.indexOf("--transaction");
    transaction = given >= 0 && given + 1 < arguments.size() ? arguments.at(given + 1) : QUuid::createUuid().toString(QUuid::Id128);
    QString executable;
    try {
        executable = helper_path();
    } catch (const std::exception &e) {
        emit failed(QString::fromStdString(e.what()));
        return false;
    }
    helper_ = executable;
    busy_ = true;
    process_.setProgram(executable);
    QStringList args{command};
    args << arguments;
    if (given < 0) args << "--transaction" << transaction;
    process_.setArguments(args);
    process_.start();
    timer_.start(timeout_ms);
    return true;
}

bool UpdaterProcess::send(const QString &command) {
    if (!busy_) return false;
    const QJsonObject message{{"protocol", 1}, {"transaction", transaction}, {"command", command}};
    return process_.write(QJsonDocument(message).toJson(QJsonDocument::Compact) + "\n") >= 0;
}

void UpdaterProcess::cancel() {
    if (busy_ && !handed_off_) {
        send("cancel");
        process_.closeWriteChannel();
    }
}

void UpdaterProcess::fail(const QString &reason) {
    if (!error_) {
        error_ = true;
        emit failed(reason);
    }
}

void UpdaterProcess::read() {
    buffer_ += process_.readAllStandardOutput();
    qsizetype newline;
    while ((newline = buffer_.indexOf('\n')) >= 0) {
        const QByteArray line = buffer_.left(newline);
        buffer_.remove(0, newline + 1);
        QString problem;
        QString event;
        QJsonObject payload;
        if (line.size() > MAX_LINE) {
            problem = "Updater message too large";
        } else {
            QJsonParseError parse{};
            const auto document = QJsonDocument::fromJson(line, &parse);
            const QJsonObject data = document.object();
            if (parse.error != QJsonParseError::NoError) {
                problem = parse.errorString();
            } else if (!document.isObject() || data.value("protocol") != QJsonValue(1) || data.value("transaction") != QJsonValue(transaction)) {
                problem = "Invalid updater protocol";
            } else if (!data.value("event").isString() || !data.value("data").isObject()) {
                problem = "Invalid updater event";
            } else {
                event = data.value("event").toString();
                payload = data.value("data").toObject();
            }
        }
        if (!problem.isEmpty()) {
            fail(problem);
            process_.kill();
            return;
        }
        if (event == "error") fail(error_text(payload));
        if (event == "downloaded" || event == "handed_off" || event == "available" || event == "up_to_date" || event == "recovered") terminal_ = true;
        if (event == "handed_off") handed_off_ = true;
        if (event == "downloaded") downloaded_ = true;
        emit event_received(event, transaction, payload);
    }
    if (buffer_.size() > MAX_LINE) {
        fail("Updater message too large");
        process_.kill();
    }
}

QString UpdaterProcess::error_text(const QJsonObject &payload) const {
    const QString code = payload.value("code").toString();
    log_warning(T("更新器报错 {code}: {message}", {{"code", payload.value("code").toVariant().toString()}, {"message", payload.value("message").toVariant().toString()}}), "Update");
    if (code == "network" || code == "source") return tr_process("Could not download the update. Check your network connection and try again.");
    if (code == "release" || code == "asset") return tr_process("The latest release has no package for this edition and architecture.");
    if (code == "archive" || code == "size" || code == "architecture") return tr_process("The downloaded package is invalid.");
    if (code == "version") return tr_process("The downloaded update does not match this installation.");
    if (code == "busy") return tr_process("Another update or recovery is already running.");
    if (code == "pending_recovery") return tr_process("An earlier update was not finished. Recover it from the .jietuba-update folder first.");
    if (code == "file_in_use" || code == "process" || code == "exit_timeout")
        return tr_process("The application did not close or its file is in use. Close it and try again.");
    if (code == "permission") return tr_process("No permission to write to the installation folder.");
    if (code == "conflict") return tr_process("The application file was changed by another program. The update was stopped.");
    if (code == "recovery_failed")
        return tr_process("The previous version could not be restored automatically. Its backup was kept in the .jietuba-update folder.");
    if (code == "cancelled" || code == "timeout") return tr_process("The update was cancelled.");
    return tr_process("The updater stopped with an error (%1).").replace("%1", code.isEmpty() ? QStringLiteral("unknown") : code);
}

void UpdaterProcess::read_stderr() { stderr_ = (stderr_ + process_.readAllStandardError()).right(8192); }

void UpdaterProcess::on_process_error(QProcess::ProcessError error) {
    fail(process_.errorString());
    if (error == QProcess::FailedToStart) {
        busy_ = false;
        timer_.stop();
        cleanup();
        emit finished();
    }
}

void UpdaterProcess::on_timeout() {
    fail("Update process timed out");
    cancel();
    process_.kill();
}

void UpdaterProcess::on_finished(int code, QProcess::ExitStatus status) {
    read();
    timer_.stop();
    busy_ = false;
    if (!error_ && (code != 0 || status != QProcess::NormalExit || !terminal_ || !buffer_.isEmpty())) {
        const QString detail = QString::fromUtf8(stderr_);
        fail(detail.isEmpty() ? QStringLiteral("Updater exited without a complete response") : detail);
    }
    cleanup();
    emit finished();
}

void UpdaterProcess::cleanup() {
    update_cache::remove_helper(helper_);
    helper_.clear();
    if (command_ == "download" && (error_ || !downloaded_)) {
        try {
            update_cache::remove_directory(QDir(update_cache::cache_root()).filePath(transaction));
        } catch (const std::exception &) {
        }
    }
}
}
