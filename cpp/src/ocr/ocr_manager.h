#pragma once
#include "ocr/snipping_tool_ocr.h"
#include <QImage>
#include <QJsonObject>
#include <QSet>
#include <QStringList>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

namespace jietuba {
// A recognition result in the reference's dict form: code 100 on success, -1 on error.
struct OcrResult {
    int code = 100;
    QString msg;
    QVector<OcrLine> data;
    double elapse = 0;
    // On error, which message the UI shows: no_model, no_runtime, model_broken, unavailable or failed.
    QString reason;
};

// One reading-order line with its bounding box.
struct OcrTextLine {
    QString text;
    double left = 0, right = 0, top = 0, bottom = 0;
};

class PpocrEngine;

// The model files a PP-OCR engine was loaded from; dict is empty when the rec model carries it.
struct PpocrFiles {
    QString det, rec, dict;
    bool operator==(const PpocrFiles &other) const { return det == other.det && rec == other.rec && dict == other.dict; }
};

// Text recognition with two engines: the Windows Snipping Tool's own OCR (oneocr)
// and PP-OCR, shipped with the full build. A singleton choosing the engine by setting.
// Recognition, loading and release all happen under one lock: releasing an engine
// while another thread still uses it makes oneocr.dll touch freed memory and crash.
class OCRManager {
public:
    static inline const QString ENGINE_AUTO = "auto";  // the Snipping Tool OCR when present, else PP-OCR
    static inline const QString ENGINE_ONEOCR = "oneocr";
    static inline const QString ENGINE_PP_RUST = "ppocr_rust";
    static inline const QStringList ENGINE_PREFERENCES = {ENGINE_AUTO, ENGINE_ONEOCR, ENGINE_PP_RUST};
    static OCRManager &instance();
    bool is_available() const;
    QStringList get_available_engines() const;
    // The engine a setting resolves to; nullopt when it is not available here.
    std::optional<QString> resolve_engine(const QString &preference) const;
    // Applies a preference at once. The engine switched away from is released now, or
    // after the recognition running at the moment; the new one loads on next use.
    bool set_engine(const QString &engine_type);
    std::optional<QString> get_current_engine() const;
    // Loads the engine; engine_type overrides the setting.
    bool initialize(const QString &engine_type = {});
    // A set cancelled stops PP-OCR at its next tile or line, and the Snipping Tool OCR at its next
    // band of a tall image, with nothing recognized.
    OcrResult recognize(const QImage &image, const std::atomic_bool *cancelled = nullptr);
    QString get_last_error() const;
    void close() { release_engine(); }
    // Releases loaded engines (tens to hundreds of MB); the next recognition reloads per setting.
    void release_engine();
    bool is_engine_loaded() const;
    QString get_memory_status() const;
    // The PP-OCR model to use; nullopt when there is none. Auto picks from the installed models
    // by catalog priority (ppocr_models::auto_choice). A chosen model that is not installed or
    // failed to load is replaced by the automatic choice without changing the setting, so it is
    // used again once it is back; only when that fails too is the broken model returned, for
    // loading to report the error.
    std::optional<QJsonObject> selected_ppocr_model() const;
    // Switches the PP-OCR model; a loaded engine reloads with it on the next recognition.
    void set_ppocr_model(const QString &model_id);
private:
    OCRManager();
    void ensure_preference();
    bool ppocr_ready() const;
    // The engine setting in force: the applied preference, else the stored setting.
    QString configured() const;
    // The model chosen in the settings; nullopt when it is not installed.
    std::optional<QJsonObject> chosen_model() const;
    // Files of a model that failed to load during this run, by path, size and modification time;
    // replacing the files makes it worth trying again.
    static QString signature(const QJsonObject &model);
    bool is_broken(const QJsonObject &model) const;
    qsizetype broken_count() const;
    // The chosen engine is missing here, or the chosen model is not installed or broke: Auto decides.
    bool falls_back(const QString &preference) const;
    // Why no engine is usable: no_model, or no_runtime when the PP-OCR runtime is missing.
    static QString missing_reason();
    void set_last_reason(const QString &reason);
    bool initialize_ppocr_rust();
    bool initialize_oneocr();
    // Releases whichever engine is not current; the caller holds the lock.
    void close_stale_engines();
    OcrResult recognize_with_ppocr_rust(const QImage &image, const std::atomic_bool *cancelled);
    OcrResult recognize_with_oneocr(const QImage &image, const std::atomic_bool *cancelled);
    static OcrResult format_result(const QVector<OcrLine> &lines, double elapse);
    static OcrResult format_empty_result();
    OcrResult format_error(const QString &error_msg = {}) const;
    void set_last_error(const QString &error);
    // The GUI thread changes these while recognition threads read them; engine_lock_ is held
    // for whole recognitions, so they have their own lock.
    mutable std::mutex state_lock_;
    QString last_error_;
    QString last_reason_;
    QSet<QString> broken_;
    // Model ids whose switch to the automatic choice was logged; each is logged once.
    mutable QSet<QString> fallback_logged_;
    std::optional<QString> preference_;  // nullopt until the setting is read
    std::optional<QString> current_engine_;
    mutable std::optional<QString> pp_model_id_;  // nullopt until the setting is read
    PpocrFiles pp_files_;                         // what pp_engine_ was loaded from
    std::unique_ptr<PpocrEngine> pp_engine_;
    std::unique_ptr<SnippingToolOcr> one_engine_;
    // Whether each engine is loaded, readable without engine_lock_.
    std::atomic_bool pp_loaded_ = false;
    std::atomic_bool one_loaded_ = false;
    std::recursive_mutex engine_lock_;
};

// "available", "missing_models" (the chosen model is not installed) or "missing_engine" (no usable onnxruntime.dll).
QString get_ppocr_status();
// Whether a file's version resource names an ONNX Runtime the PP-OCR engine can load.
bool ppocr_runtime_supported(const QString &dll);
// Tests hide or show the Snipping Tool OCR; nullopt goes back to detecting it.
void set_oneocr_available_for_test(std::optional<bool> available);
// Tests stand in for the engines with a function, which runs on the recognition thread; an empty
// one goes back to the engines.
void set_recognizer_for_test(std::function<OcrResult(const QImage &)> recognizer);
bool is_ocr_available();
QStringList get_available_engines();
bool set_ocr_engine(const QString &engine_type);
// A subfolder name under models/, empty for the default model.
void set_ppocr_model(const QString &model_id);
std::optional<QString> get_current_engine();
bool initialize_ocr(const QString &engine_type = {});
OcrResult recognize_text(const QImage &image, const std::atomic_bool *cancelled = nullptr);
// Frees memory after pins close or when OCR goes unused for long.
void release_ocr_engine();
QString get_ocr_memory_status();
// Reading-order lines: grouped by vertical centre (top to bottom), sorted by x within a
// line and joined with spaces. Kept as lines because layout needs line positions. A tilted page
// is first turned level by its lines' median direction, so the positions are in that frame.
QVector<OcrTextLine> group_ocr_lines(const OcrResult &result);
QString format_ocr_result_text(const OcrResult &result, const QString &separator = "\n");
}
