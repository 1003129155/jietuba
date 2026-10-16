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
}

UpdateController::UpdateController(UpdateHost *host, QObject *parent, UpdaterProcess *runner_) : QObject(parent), runner(runner_ ? runner_ : new UpdaterProcess), host_(host) {
    runner->setParent(this);
    connect(runner, &UpdaterProcess::event_received, this, &UpdateController::on_event);
    connect(runner, &UpdaterProcess::failed, this, &UpdateController::on_error);
    connect(runner, &UpdaterProcess::finished, this, &UpdateController::on_finished);
}

UpdateController::~UpdateController() { delete dialog; }

void UpdateController::present(const ReleaseInfo &value) {
    if (runner->busy()) {
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
    discard_download();
    release = value;
    downloaded.clear();
    dialog = new UpdateDialog(value);
    connect(dialog, &UpdateDialog::update_requested, this, &UpdateController::start);
    connect(dialog, &UpdateDialog::cancel_requested, this, &UpdateController::cancel);
    dialog->show();
}

QStringList UpdateController::arguments() const {
    return {"--install-exe", QDir::toNativeSeparators(QCoreApplication::applicationFilePath()), "--current-version", APP_VERSION, "--variant", app_variant()};
}

void UpdateController::start() {
    if (runner->busy() || !dialog || committed_) return;
    failed_ = cancelled_ = false;
    if (!updater_bundled()) {
        on_error(tr_controller("One-click updates are available in the packaged application."));
        return;
    }
    dialog->set_working(true);
    if (!downloaded.isEmpty()) {
        install();
        return;
    }
    try {
        const QString path = QDir(update_cache::cache_subdirectory("requests")).filePath(QUuid::createUuid().toString(QUuid::Id128) + ".json");
        request_ = path;
        const QJsonObject data{{"tag_name", release->tag_name}, {"title", release->title},   {"notes", release->notes},      {"url", release->url},
                               {"asset_name", release->asset_name}, {"arch", release->arch}, {"artifact", release->artifact}};
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(data).toJson(QJsonDocument::Compact)) < 0) throw std::runtime_error("Cannot write " + path.toStdString());
        file.close();
        mode = "download";
        dialog->status->setText(tr_controller("Downloading update..."));
        if (!runner->start("download", arguments() << "--release-file" << path)) {
            remove_request();
            dialog->set_working(false);
        }
    } catch (const std::exception &e) {
        remove_request();
        on_error(QString::fromStdString(e.what()));
        if (dialog) dialog->set_working(false);
    }
}

void UpdateController::install() {
    const QString reason = host_->update_busy_reason();
    if (!reason.isEmpty()) {
        on_error(reason);
        dialog->set_working(false);
        return;
    }
    mode = "apply";
    dialog->progress->setRange(0, 0);
    dialog->status->setText(tr_controller("Preparing to restart..."));
    // The worker shows this after the application has exited, so it is translated here.
    const QString failure = tr_controller("The update was not completed. The previous version and its recovery record were kept.");
    // Reuse the download transaction to install the same staged release.
    const QStringList args = arguments() << "--transaction" << downloaded << "--parent-pid" << QString::number(QCoreApplication::applicationPid())
                                         << "--failure-message" << failure;
    if (!runner->start("apply", args, 105000)) dialog->set_working(false);
}

void UpdateController::on_event(const QString &event, const QString &transaction, const QJsonObject &data) {
    if (event == "progress") {
        const double total = data.value("total").toDouble();
        const double received = data.value("received").toDouble();
        dialog->progress->setRange(0, total > 0 ? 100 : 0);
        if (total > 0) dialog->progress->setValue(std::min(100, static_cast<int>(received * 100 / total)));
    } else if (event == "downloaded") {
        downloaded = transaction;
    } else if (event == "ready") {
        const auto [ok, reason] = host_->prepare_for_update();
        if (ok && runner->send("go")) {
            dialog->committed = true;
            dialog->set_working(true);
        } else {
            on_error(reason.isEmpty() ? tr_controller("Update handoff failed.") : reason);
            runner->cancel();
        }
    } else if (event == "handed_off") {
        committed_ = true;
    }
}

void UpdateController::on_error(const QString &reason) {
    failed_ = true;
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
    if (mode == "download" && (failed_ || cancelled_)) discard_download();
    if (mode == "download" && !downloaded.isEmpty() && !failed_ && !cancelled_) install();
}

void UpdateController::cancel() {
    if (committed_ || (dialog && dialog->committed)) return;
    cancelled_ = true;
    runner->cancel();
    host_->cancel_update_preparation();
    if (!runner->busy()) discard_download();
}

void UpdateController::remove_request() {
    if (!request_.isEmpty()) {
        update_cache::remove_request(request_);
        request_.clear();
    }
}

void UpdateController::discard_download() {
    if (!downloaded.isEmpty()) {
        try {
            update_cache::remove_directory(QDir(update_cache::cache_root()).filePath(downloaded));
        } catch (const std::exception &) {
        }
        downloaded.clear();
    }
}
}
