#include "core/update_controller.h"
#include "core/update_cache.h"
#include "main_app.h"
#include "ui/update_dialog.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QUuid>
#include <algorithm>

namespace jietuba {
namespace {
QString tr_controller(const char *text) { return QCoreApplication::translate("UpdateController", text); }
// An install at start-up holds start-up, so it gives up well before an interactive one.
constexpr int BACKGROUND_INSTALL_TIMEOUT_MS = 45000;
}

UpdateController::UpdateController(UpdateHost *host, QObject *parent, UpdaterProcess *runner_) : QObject(parent), runner(runner_ ? runner_ : new UpdaterProcess), host_(host) {
    runner->setParent(this);
    connect(runner, &UpdaterProcess::event_received, this, &UpdateController::on_event);
    connect(runner, &UpdaterProcess::failed, this, &UpdateController::on_error);
    connect(runner, &UpdaterProcess::finished, this, &UpdateController::on_finished);
}

UpdateController::~UpdateController() { delete dialog; }

void UpdateController::show_dialog(const ReleaseInfo &value) {
    dialog = new UpdateDialog(value);
    connect(dialog, &UpdateDialog::update_requested, this, &UpdateController::start);
    connect(dialog, &UpdateDialog::cancel_requested, this, &UpdateController::cancel);
    dialog->show();
}

void UpdateController::present(const ReleaseInfo &value) {
    const bool same_package = release && release->same_package(value);
    if (runner->busy()) {
        // A background download of this release becomes the user's: it installs once it finishes.
        if (background_ && mode == "download" && same_package && !dialog) {
            background_ = false;
            release = value;
            show_dialog(value);
            dialog->set_working(true);
            dialog->status->setText(tr_controller("Downloading update..."));
            return;
        }
        if (dialog) {
            dialog->show();
            dialog->raise();
        }
        return;
    }
    if (dialog && release == value) {
        dialog->show();
        dialog->raise();
        return;
    }
    if (dialog) dialog->deleteLater();
    if (!same_package) discard_download();
    release = value;
    show_dialog(value);
}

QStringList UpdateController::arguments() const {
    return {"--install-exe", QDir::toNativeSeparators(QCoreApplication::applicationFilePath()), "--current-version", APP_VERSION, "--variant", app_variant()};
}

bool UpdateController::begin_download() {
    kept_ = false;
    try {
        const QString path = QDir(update_cache::cache_subdirectory("requests")).filePath(QUuid::createUuid().toString(QUuid::Id128) + ".json");
        request_ = path;
        QJsonObject data{{"tag_name", release->tag_name}, {"title", release->title},   {"notes", release->notes},      {"url", release->url},
                         {"asset_name", release->asset_name}, {"arch", release->arch}, {"artifact", release->artifact}};
        if (!release->published_at.isEmpty()) data.insert("published_at", release->published_at);
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(data).toJson(QJsonDocument::Compact)) < 0) throw std::runtime_error("Cannot write " + path.toStdString());
        file.close();
        mode = "download";
        if (dialog) dialog->status->setText(tr_controller("Downloading update..."));
        if (runner->start("download", arguments() << "--release-file" << path)) return true;
        remove_request();
    } catch (const std::exception &e) {
        remove_request();
        on_error(QString::fromStdString(e.what()));
    }
    return false;
}

void UpdateController::start() {
    if (runner->busy() || !dialog || committed_) return;
    failed_ = cancelled_ = background_ = false;
    if (!updater_bundled()) {
        on_error(tr_controller("One-click updates are available in the packaged application."));
        return;
    }
    dialog->set_working(true);
    if (!downloaded.isEmpty()) {
        install();
        return;
    }
    if (!begin_download() && dialog) dialog->set_working(false);
}

bool UpdateController::download_in_background(const ReleaseInfo &value) {
    if (runner->busy() || committed_ || !updater_bundled() || has_download(value)) return false;
    // The user is looking at a release; this waits for the next check.
    if (dialog && dialog->isVisible()) return false;
    if (dialog) dialog->deleteLater();
    dialog = nullptr;
    discard_download();
    release = value;
    failed_ = cancelled_ = false;
    background_ = true;
    if (begin_download()) return true;
    background_ = false;
    return false;
}

void UpdateController::cancel_background() {
    if (background_ && runner->busy() && mode == "download") cancel();
}

bool UpdateController::install_in_background(const ReleaseInfo &value, const QString &transaction) {
    if (runner->busy() || committed_ || dialog || !updater_bundled()) return false;
    release = value;
    downloaded = transaction;
    kept_ = true;
    failed_ = cancelled_ = false;
    background_ = true;
    if (install(BACKGROUND_INSTALL_TIMEOUT_MS)) return true;
    background_ = false;
    discard_download();
    return false;
}

bool UpdateController::install(int timeout_ms) {
    const QString reason = host_->update_busy_reason();
    if (!reason.isEmpty()) {
        on_error(reason);
        if (dialog) dialog->set_working(false);
        return false;
    }
    mode = "apply";
    if (dialog) {
        dialog->progress->setRange(0, 0);
        dialog->status->setText(tr_controller("Preparing to restart..."));
    }
    // The worker shows this after the application has exited, so it is translated here.
    const QString failure = tr_controller("The update was not completed. The previous version and its recovery record were kept.");
    // Reuse the download transaction to install the same staged release.
    const QStringList args = arguments() << "--transaction" << downloaded << "--parent-pid" << QString::number(QCoreApplication::applicationPid())
                                         << "--failure-message" << failure;
    if (runner->start("apply", args, timeout_ms)) return true;
    if (dialog) dialog->set_working(false);
    return false;
}

void UpdateController::on_event(const QString &event, const QString &transaction, const QJsonObject &data) {
    if (event == "progress") {
        if (!dialog) return;
        const double total = data.value("total").toDouble();
        const double received = data.value("received").toDouble();
        dialog->progress->setRange(0, total > 0 ? 100 : 0);
        if (total > 0) dialog->progress->setValue(std::min(100, static_cast<int>(received * 100 / total)));
    } else if (event == "downloaded") {
        downloaded = transaction;
    } else if (event == "ready") {
        const auto [ok, reason] = host_->prepare_for_update();
        if (ok && runner->send("go")) {
            if (dialog) {
                dialog->committed = true;
                dialog->set_working(true);
            }
        } else {
            on_error(reason.isEmpty() ? tr_controller("Could not start installing the update.") : reason);
            runner->cancel();
        }
    } else if (event == "handed_off") {
        committed_ = true;
    }
}

void UpdateController::on_error(const QString &reason) {
    failed_ = true;
    failure_ = reason;
    host_->cancel_update_preparation();
    if (dialog) {
        dialog->committed = false;
        dialog->status->setText(tr_controller("Update failed: %1").replace("%1", reason));
    }
}

void UpdateController::on_finished() {
    remove_request();
    if (committed_) {
        if (dialog) dialog->finish_handoff();
        host_->quit_app();
        return;
    }
    if (dialog) dialog->set_working(false);
    const bool ok = !failed_ && !cancelled_;
    if (mode == "download" && !ok) discard_download();
    if (background_) {
        background_ = false;
        if (mode == "download") {
            kept_ = ok && !downloaded.isEmpty();
            emit background_download_finished(kept_);
        } else {
            // A download that would not install is not tried again.
            kept_ = false;
            discard_download();
            emit background_install_failed(failure_.isEmpty() ? tr_controller("Could not start installing the update.") : failure_);
        }
        return;
    }
    if (mode == "download" && !downloaded.isEmpty() && ok) install();
}

void UpdateController::cancel() {
    if (committed_ || (dialog && dialog->committed)) return;
    cancelled_ = true;
    runner->cancel();
    host_->cancel_update_preparation();
    if (!runner->busy() && !kept_) discard_download();
}

void UpdateController::remove_request() {
    if (!request_.isEmpty()) {
        update_cache::remove_request(request_);
        request_.clear();
    }
}

void UpdateController::discard_download() {
    kept_ = false;
    if (!downloaded.isEmpty()) {
        try {
            update_cache::remove_directory(QDir(update_cache::cache_root()).filePath(downloaded));
        } catch (const std::exception &) {
        }
        downloaded.clear();
    }
}
}
