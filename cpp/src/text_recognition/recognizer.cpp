#include "text_recognition/recognizer.h"
#include "core/logger.h"
#include "core/platform_utils.h"
#include <QSet>
#include <algorithm>

namespace jietuba {
namespace {
QSet<RecognizeThread *> &running() {
    static QSet<RecognizeThread *> threads;
    return threads;
}

// The thread finished (delivered on the GUI thread): unregister it and let the event
// loop delete it.
void retire(RecognizeThread *thread) {
    running().remove(thread);
    thread->deleteLater();
    // Gives back the pages the engine touched during inference.
    request_trim_working_set();
}
}

void RecognizeThread::run() {
    QVector<OcrTextLine> lines;
    QString reason = recognition_reason::FAILED;
    try {
        if (!is_ocr_available()) {
            reason = get_ppocr_status() == "missing_models" ? recognition_reason::NO_MODEL : recognition_reason::UNAVAILABLE;
        } else if (const auto result = recognize_text(image_); result.code == -1) {
            reason = result.reason.isEmpty() ? recognition_reason::FAILED : result.reason;
        } else {
            lines = group_ocr_lines(result);
            const bool has_text = std::any_of(lines.begin(), lines.end(), [](const OcrTextLine &line) { return !line.text.trimmed().isEmpty(); });
            reason = has_text ? QString() : recognition_reason::NO_TEXT;
        }
    } catch (const std::exception &e) {
        log_error(T("文字识别失败: {e}", {{"e", QString::fromUtf8(e.what())}}), "OCR");
    }
    // Released once recognized; the thread object waits for the GUI thread to collect it.
    image_ = QImage();
    if (!cancelled_) emit recognized(lines, reason);
}

RecognizeThread *recognize_async(const QImage &image, QObject *context, RecognizedCallback on_recognized) {
    auto *thread = new RecognizeThread(image);
    QObject::connect(thread, &RecognizeThread::recognized, context, std::move(on_recognized));
    QObject::connect(thread, &QThread::finished, thread, [thread] { retire(thread); });
    running().insert(thread);
    thread->start();
    log_debug(T("文字识别线程已启动"), "OCR");
    return thread;
}

void shutdown_recognition(int timeout_ms) {
    for (auto *thread : running().values()) {
        thread->cancel();
        if (thread->isRunning() && !thread->wait(std::max(0, timeout_ms))) log_warning(T("退出时文字识别线程未在期限内结束"), "OCR");
    }
}
}
