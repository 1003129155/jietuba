#include "ocr/ppocr_models.h"
#include "core/i18n.h"
#include "core/logger.h"
#include "core/native.h"
#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QTemporaryFile>
#include <memory>
#include <stdexcept>

namespace jietuba {
namespace {
QByteArray native_path(const QString &path) { return QDir::toNativeSeparators(path).toUtf8(); }

// Takes a string returned by the jb_ppocr_models_* functions.
QJsonDocument take_json(char *text) {
    if (!text) return {};
    const auto document = QJsonDocument::fromJson(QByteArray(text));
    jb_ppocr_text_free(text);
    return document;
}

QString tr_models(const char *text) { return QCoreApplication::translate("ModelUpdaterProcess", text); }

constexpr const char *ARCH =
#if defined(_M_ARM64)
    "arm64";
#else
    "x64";
#endif
}

namespace ppocr_models {
namespace {
QString &test_root() {
    static QString root;
    return root;
}
}

QString models_root() { return test_root().isEmpty() ? QDir(QCoreApplication::applicationDirPath()).filePath("models") : test_root(); }

void set_models_root_for_test(const QString &root) { test_root() = root; }

QString runtime_path() { return QDir(models_root()).filePath("onnxruntime.dll"); }

QJsonObject catalog() { return take_json(jb_ppocr_models_catalog()).object(); }

std::optional<QJsonObject> catalog_entry(const QString &catalog_id) {
    for (const auto &value : catalog().value("models").toArray())
        if (value.toObject().value("id").toString() == catalog_id) return value.toObject();
    return std::nullopt;
}

std::optional<QJsonObject> runtime_entry() {
    for (const auto &value : catalog().value("runtimes").toArray())
        if (value.toObject().value("arch").toString() == ARCH) return value.toObject();
    return std::nullopt;
}

QJsonArray installed() { return take_json(jb_ppocr_models_discover(native_path(models_root()).constData())).array(); }

std::optional<QJsonObject> resolve(const QString &model_id) {
    const auto document = take_json(jb_ppocr_models_resolve(native_path(models_root()).constData(), model_id.toUtf8().constData()));
    if (!document.isObject()) return std::nullopt;
    return document.object();
}

std::optional<QJsonObject> auto_choice() {
    const auto document = take_json(jb_ppocr_models_auto_choice(native_path(models_root()).constData()));
    if (!document.isObject()) return std::nullopt;
    return document.object();
}

QJsonObject import_model(const QString &name, const QString &det, const QString &rec, const QString &dict) {
    char error[1024] = {};
    const QByteArray dict_path = native_path(dict);
    char *text = jb_ppocr_models_import(native_path(models_root()).constData(), name.toUtf8().constData(), native_path(det).constData(), native_path(rec).constData(),
                                        dict.isEmpty() ? nullptr : dict_path.constData(), error, sizeof error);
    if (!text) throw std::runtime_error(error);
    return take_json(text).object();
}

void remove(const QString &model_id) {
    char error[1024] = {};
    if (!jb_ppocr_models_remove(native_path(models_root()).constData(), model_id.toUtf8().constData(), error, sizeof error)) throw std::runtime_error(error);
}

void clean_staging() {
    if (QDir(models_root()).exists()) jb_ppocr_models_clean_staging(native_path(models_root()).constData());
}
}

QString ModelUpdaterProcess::error_text(const QJsonObject &payload) const {
    const QString code = payload.value("code").toString();
    log_warning(T("模型下载报错 {code}: {message}", {{"code", payload.value("code").toVariant().toString()}, {"message", payload.value("message").toVariant().toString()}}), "OCR");
    if (code == "network" || code == "source")
        return runtime ? tr_models("Could not download ONNX Runtime. Check your network connection and try again.")
                       : tr_models("Could not download the model. Check your network connection and try again.");
    if (code == "hash" || code == "size")
        return runtime ? tr_models("The downloaded ONNX Runtime file is damaged. Try again.") : tr_models("The downloaded model file is damaged. Try again.");
    if (code == "permission") return tr_models("No permission to write to the models folder.");
    if (code == "cancelled" || code == "timeout") return tr_models("The download was cancelled.");
    QString text = runtime ? tr_models("The ONNX Runtime download stopped with an error (%1).") : tr_models("The model download stopped with an error (%1).");
    return text.replace("%1", code.isEmpty() ? QStringLiteral("unknown") : code);
}

ModelDownload::ModelDownload(QObject *parent) : QObject(parent) { set_runner(new ModelUpdaterProcess(this)); }

ModelDownload::~ModelDownload() { remove_request(); }

void ModelDownload::set_runner(UpdaterProcess *runner) {
    if (runner_ && runner_->parent() == this) runner_->deleteLater();
    runner_ = runner;
    connect(runner_, &UpdaterProcess::event_received, this, &ModelDownload::on_event);
    connect(runner_, &UpdaterProcess::failed, this, &ModelDownload::on_failed);
    connect(runner_, &UpdaterProcess::finished, this, &ModelDownload::on_finished);
}

bool ModelDownload::busy() const { return runner_->busy(); }

bool ModelDownload::start(const QString &catalog_id) {
    if (busy()) return false;
    QJsonArray files;
    if (catalog_id == RUNTIME) {
        const auto runtime = ppocr_models::runtime_entry();
        if (!runtime) return false;
        files.append(runtime->value("file"));
    } else {
        const auto entry = ppocr_models::catalog_entry(catalog_id);
        if (!entry) return false;
        files = entry->value("files").toArray();
    }
    QTemporaryFile request(QDir(QDir::tempPath()).filePath("jietuba-model-XXXXXX.json"));
    request.setAutoRemove(false);
    if (!request.open()) return false;
    request.write(QJsonDocument(QJsonObject{{"staging", catalog_id}, {"files", files}}).toJson(QJsonDocument::Compact));
    request.close();
    request_ = request.fileName();
    catalog_id_ = catalog_id;
    error_.clear();
    if (auto *process = qobject_cast<ModelUpdaterProcess *>(runner_)) process->runtime = catalog_id == RUNTIME;
    const QString install_exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    if (!runner_->start("download-model", {"--install-exe", install_exe, "--model-file", QDir::toNativeSeparators(request_)}, 3600000)) {
        remove_request();
        return false;
    }
    return true;
}

void ModelDownload::cancel() { runner_->cancel(); }

void ModelDownload::on_event(const QString &event, const QString &, const QJsonObject &data) {
    if (event == "progress") {
        emit progress(data.value("received").toVariant().toLongLong(), data.value("total").toVariant().toLongLong());
    } else if (event == "downloaded") {
        char error[1024] = {};
        const QByteArray root = native_path(ppocr_models::models_root());
        if (catalog_id_ == RUNTIME) {
            if (!jb_ppocr_models_finish_runtime_download(root.constData(), error, sizeof error)) return on_failed(QString::fromUtf8(error));
            emit finished(QJsonObject{{"id", RUNTIME}});
            return;
        }
        char *text = jb_ppocr_models_finish_download(root.constData(), catalog_id_.toUtf8().constData(), error, sizeof error);
        if (!text) return on_failed(QString::fromUtf8(error));
        emit finished(take_json(text).object());
    }
}

void ModelDownload::on_failed(const QString &reason) {
    if (!error_.isEmpty()) return;
    error_ = reason;
    emit failed(reason);
}

void ModelDownload::on_finished() {
    remove_request();
    emit stopped();
}

void ModelDownload::remove_request() {
    if (request_.isEmpty()) return;
    QFile::remove(request_);
    request_.clear();
}
}
