#pragma once
#include "core/http_client.h"
#include "translation/overlay_translation.h"
#include "translation/service.h"
#include <QImage>
#include <QThread>
#include <QVariantMap>
#include <atomic>
#include <memory>
#include <optional>
#include <vector>

namespace jietuba {
// Recognizes the capture and translates its paragraphs off the GUI thread: both the OCR
// engine and the providers block, and the capture window has to stay responsive while the
// progress bar runs. The image is a value, so nothing here touches a widget.
class OverlayTranslationWorker : public QThread {
    Q_OBJECT
public:
    // The providers are resolved and the quota checked here, on the GUI thread, since both
    // read QSettings; run() only makes the network calls.
    OverlayTranslationWorker(const QImage &image, const QVariantMap &params, std::shared_ptr<TranslationService> service, QObject *parent = nullptr);
    // Before start(): the capture is already recognized. Recognition is skipped, and only
    // the blocks without a translation are sent.
    void set_blocks(const QVector<OverlayTextBlock> &blocks);
    // Before start(): false leaves result() null, for a caller that draws the blocks itself.
    void set_paint_result(bool paint) { paint_result_ = paint; }
    // Stops the OCR at its next band or tile and the requests in flight at once; no result
    // is emitted afterwards.
    void abort();
    const QString &target_lang() const { return target_lang_; }
    // Read on the GUI thread once finished_signal arrived.
    bool succeeded() const { return succeeded_; }
    QString error_text() const { return error_; }
    const QVector<OverlayTextBlock> &blocks() const { return blocks_; }
    // The image with the translations painted on; null when no block changed.
    QImage result() const { return result_; }
    int painted_blocks() const { return painted_blocks_; }
    // Paragraphs the service did not translate, left as they were.
    int failed_blocks() const { return failed_blocks_; }
    // What to tell the user about them, with the first one's reason; empty when there are none.
    QString failure_notice() const;
    // Source characters sent to the provider, for the monthly quota bookkeeping.
    qint64 translated_chars() const { return translated_chars_; }
signals:
    // total is 0 while the count is not known yet (recognition).
    void progress_changed(int done, int total);
    // The capture's paragraphs, once recognition is done; not sent after set_blocks().
    void recognized(const QVector<jietuba::OverlayTextBlock> &blocks);
    // One paragraph's request is over; translation is empty when it failed.
    void block_translated(int index, const QString &translation);
    void finished_signal(bool success);
protected:
    void run() override;
private:
    QImage image_;
    QString target_lang_;
    QString split_sentences_;
    bool preserve_formatting_ = true;
    std::shared_ptr<TranslationService> service_;
    std::vector<std::unique_ptr<TranslationProvider>> providers_;
    QString configuration_error_;
    std::optional<TranslationResult> quota_error_;
    QVector<OverlayTextBlock> blocks_;
    bool recognized_ = false;
    bool paint_result_ = true;
    QImage result_;
    int painted_blocks_ = 0;
    int failed_blocks_ = 0;
    QString failure_reason_;
    QString error_;
    bool succeeded_ = false;
    qint64 translated_chars_ = 0;
    std::atomic_bool cancelled_ = false;
    // Ends the exchanges in flight when the capture is cancelled or closed.
    HttpCancellation cancellation_;
};
}
