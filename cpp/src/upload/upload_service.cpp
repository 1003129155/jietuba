#include "upload/upload_service.h"
#include "core/background_tasks.h"
#include "core/file_names.h"
#include "core/i18n.h"
#include "core/logger.h"
#include "settings/tool_settings.h"
#include "ui/dialogs.h"
#include "ui/toast.h"
#include <QClipboard>
#include <QGuiApplication>
#include <QPointer>
#include <stdexcept>

namespace jietuba::upload {
namespace {
QString upload_tr(const char *text) { return i18n_tr(text, "CloudUpload"); }

struct Finished {
    PutOutcome outcome;
    QString link;
};

// Runs work on a background thread and hands its result to finish on the main thread, unless the owner (when
// given) was deleted meanwhile. The result is posted to the application object, which outlives the thread; a
// deleted owner cannot be posted to.
void run_background(QObject *owner, std::function<Finished()> work, std::function<void(const Finished &)> finish) {
    const QPointer<QObject> alive(owner);
    const bool owned = owner != nullptr;
    const auto post = [alive, owned, finish](const Finished &finished) {
        if (!qApp) return;
        QMetaObject::invokeMethod(qApp, [alive, owned, finish, finished] {
            if (!owned || alive) finish(finished);
        }, Qt::QueuedConnection);
    };
    try {
        background_tasks::start_thread(
            [work = std::move(work), post] {
                Finished finished;
                try {
                    finished = work();
                } catch (const std::exception &) {
                    finished.outcome.kind = PutOutcome::NetworkError;
                }
                post(finished);
            },
            "CloudUpload");
    } catch (const std::runtime_error &) {
        // New threads do not start while an update is being prepared.
        Finished finished;
        finished.outcome.kind = PutOutcome::NetworkError;
        finish(finished);
    }
}

void log_outcome(const PutOutcome &outcome) {
    if (outcome.ok()) log_info(T("云上传: 已上传"), "Upload");
    else log_warning(T("云上传: 失败 kind={kind} status={status} code={code}", {{"kind", static_cast<int>(outcome.kind)}, {"status", outcome.status}, {"code", outcome.code}}), "Upload");
}
}

std::optional<Target> default_target(ToolSettingsManager &config) {
    const auto targets = parse_targets(config.get_upload_targets());
    if (const auto *target = find_target(targets, config.get_upload_default_target())) return *target;
    return std::nullopt;
}

void show_no_target_hint() { show_toast(upload_tr("Add an upload target in Settings first")); }

bool start_upload(const QImage &image, ToolSettingsManager &config, Source source, std::function<void(const Report &)> done) {
    const auto target = default_target(config);
    if (!target || image.isNull()) return false;
    const QString stem = file_names::stem(config, source == Source::Pin ? file_names::Pin : file_names::Screenshot);
    const auto settings = read_encode_settings(config);
    const auto format = parse_link_format(config.get_upload_link_format());
    const QString random = random_suffix();
    const auto now = QDateTime::currentDateTime();
    const QPointer<Toast> toast(new Toast(upload_tr("Uploading...")));
    run_background(
        nullptr,
        [target = *target, image = QImage(image), settings, stem, random, now]() mutable {
            Finished finished;
            const auto encoded = encode_for_upload(image, settings);
            // The upload may take long; it needs only the file.
            image = QImage();
            if (encoded.bytes.isEmpty()) {
                finished.outcome.kind = PutOutcome::EncodeFailed;
                return finished;
            }
            const QString key = make_object_key(target.path_prefix, stem, random, encoded.extension, now);
            finished.outcome = put_object(target, key, encoded.bytes, encoded.content_type);
            if (finished.outcome.ok()) finished.link = public_link(target, key);
            return finished;
        },
        [toast, format, done = std::move(done)](const Finished &finished) {
            log_outcome(finished.outcome);
            Report report;
            report.outcome = finished.outcome;
            if (finished.outcome.ok()) {
                report.link = format_link(finished.link, format);
                report.message = upload_tr("Uploaded, link copied");
                QGuiApplication::clipboard()->setText(report.link);
                if (toast) toast->finish(report.message);
            } else {
                report.message = outcome_detail(finished.outcome);
                if (toast) toast->finish(outcome_summary(finished.outcome));
                if (!done) show_modeless_warning_dialog(nullptr, upload_tr("Upload"), report.message);
            }
            if (done) done(report);
        });
    return true;
}

void upload_test_image(const Target &target, QObject *context, std::function<void(const Report &)> done) {
    const QString random = random_suffix();
    const auto now = QDateTime::currentDateTime();
    run_background(
        context,
        [target, random, now] {
            Finished finished;
            QImage pixel(1, 1, QImage::Format_RGB32);
            pixel.fill(Qt::white);
            const auto encoded = encode_for_upload(pixel, {"PNG", -1});
            const QString key = make_object_key(target.path_prefix, "jietuba-test", random, ".png", now);
            finished.outcome = put_object(target, key, encoded.bytes, encoded.content_type);
            if (finished.outcome.ok()) finished.link = public_link(target, key);
            return finished;
        },
        [done = std::move(done)](const Finished &finished) {
            Report report;
            report.outcome = finished.outcome;
            report.link = finished.link;
            report.message = finished.outcome.ok() ? upload_tr("Test upload succeeded") : outcome_detail(finished.outcome);
            if (done) done(report);
        });
}
}
