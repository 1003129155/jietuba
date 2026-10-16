#pragma once
#include "ocr/ocr_manager.h"
#include <QImage>
#include <QThread>
#include <atomic>
#include <functional>

// Runs one image through the OCR engine on a worker thread and reports (lines,
// failure reason key).
//
// Recognition is one uninterruptible native call: cancel only stops the result
// from being reported. Process teardown with a thread still inside it aborts the
// process, so every thread is registered and shutdown_recognition waits for them.
//
// Failures report a reason key, not a sentence: sentences need the translator
// installed on the GUI thread, which the worker must not touch.
namespace jietuba {
namespace recognition_reason {
// This build has no OCR engine.
inline const QString UNAVAILABLE = "unavailable";
// The engine ran but found no text.
inline const QString NO_TEXT = "no_text";
// Recognition itself failed.
inline const QString FAILED = "failed";
// PP-OCR is there but no model is installed, and there is no Snipping Tool OCR.
inline const QString NO_MODEL = "no_model";
// The model files do not load, and the automatic choice has nothing else.
inline const QString MODEL_BROKEN = "model_broken";
}

// One recognition. Holds only a QImage (a value type), so closing the window or
// releasing the screenshot never leaves it with a dangling widget.
class RecognizeThread : public QThread {
    Q_OBJECT
public:
    explicit RecognizeThread(const QImage &image) : image_(image) {}
    // Drops this recognition's result; the recognition itself cannot be interrupted.
    void cancel() { cancelled_ = true; }
signals:
    // Lines rather than text: layout needs line positions to judge wrapping. The
    // reason is empty on success.
    void recognized(const QVector<jietuba::OcrTextLine> &lines, const QString &reason);
protected:
    void run() override;
private:
    QImage image_;
    std::atomic_bool cancelled_ = false;
};

using RecognizedCallback = std::function<void(const QVector<OcrTextLine> &, const QString &)>;

// Recognizes image in the background and reports to on_recognized. The connection
// lives as long as context, so a window closed first is never called. It is made
// before start: without an engine recognition fails before the caller even has the
// returned thread.
RecognizeThread *recognize_async(const QImage &image, QObject *context, RecognizedCallback on_recognized);

// Waits for running recognition threads before exit. Cancelling alone is not enough:
// the wait is what makes exit safe. Threads that do not finish stay registered,
// since destroying a running QThread crashes.
void shutdown_recognition(int timeout_ms = 3000);
}
