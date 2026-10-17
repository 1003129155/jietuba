#pragma once
#include "core/updater_process.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <optional>

namespace jietuba {
// PP-OCR models in models/ next to the executable, the built-in download catalog, importing
// and removal. The rules live in ppocr_rust::models (shared with the reference); this side
// finds the folder and converts JSON. Downloads go through the updater helper.
namespace ppocr_models {
QString models_root();
// Tests point models/ at a temporary folder; empty restores the default.
void set_models_root_for_test(const QString &root);
// models/onnxruntime.dll, which this build loads at run time.
QString runtime_path();
QJsonObject catalog();
std::optional<QJsonObject> catalog_entry(const QString &catalog_id);
// The catalog's ONNX Runtime for this architecture; nullopt when the catalog has none.
std::optional<QJsonObject> runtime_entry();
// Installed models, the default one (empty id) first.
QJsonArray installed();
// An installed model by id; nullopt when it is not installed.
std::optional<QJsonObject> resolve(const QString &model_id);
// The installed model Auto uses: by catalog priority, models added by hand last; nullopt when
// none is installed.
std::optional<QJsonObject> auto_choice();
// Copies and validates user-chosen files; throws std::runtime_error, leaving nothing behind.
QJsonObject import_model(const QString &name, const QString &det, const QString &rec, const QString &dict = {});
// Throws std::runtime_error; the default model (empty id) removes its two files from models/.
void remove(const QString &model_id);
void clean_staging();
}

class ModelUpdaterProcess : public UpdaterProcess {
    Q_OBJECT
public:
    using UpdaterProcess::UpdaterProcess;
    // True while the run downloads the ONNX Runtime instead of a model, so errors name it.
    bool runtime = false;
    QString error_text(const QJsonObject &payload) const override;
};

// Downloads one catalog model, or the catalog's ONNX Runtime (catalog id "runtime").
// progress reports bytes over all files; stopped follows every run, finished or not.
class ModelDownload : public QObject {
    Q_OBJECT
public:
    static inline const QString RUNTIME = "runtime";
    explicit ModelDownload(QObject *parent = nullptr);
    ~ModelDownload() override;
    bool busy() const;
    QString catalog_id() const { return catalog_id_; }
    bool start(const QString &catalog_id);
    void cancel();
    // Tests replace the helper process.
    void set_runner(UpdaterProcess *runner);
signals:
    void progress(qint64 received, qint64 total);
    void finished(const QJsonObject &model);
    void failed(const QString &reason);
    void stopped();
private:
    void on_event(const QString &event, const QString &transaction, const QJsonObject &data);
    void on_failed(const QString &reason);
    void on_finished();
    void remove_request();
    UpdaterProcess *runner_ = nullptr;
    QString request_;
    QString catalog_id_;
    QString error_;
};
}
