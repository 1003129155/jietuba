#include "translation/overlay_translation_worker.h"
#include "core/i18n.h"
#include "core/logger.h"
#include <stdexcept>

namespace jietuba {
namespace {
QString overlay_tr(const char *text) { return i18n_tr(text, "OverlayTranslation"); }

// An LLM reply costs a round trip plus whatever the model thinks before answering: one
// call per paragraph turns a long capture into minutes. Both limits matter, since a page
// of short labels and a few long paragraphs should cost about the same request.
constexpr int BATCH_MAX_TEXTS = 8;
constexpr int BATCH_MAX_CHARS = 900;

// A batch that failed for these reasons will fail text by text too, and each retry costs
// another timeout: report at once instead of crawling through the capture.
bool fatal_batch_error(const TranslationErrorCode code) {
    return code == TranslationErrorCode::NOT_CONFIGURED || code == TranslationErrorCode::AUTH_FAILED ||
           code == TranslationErrorCode::QUOTA_EXCEEDED || code == TranslationErrorCode::RATE_LIMITED ||
           code == TranslationErrorCode::NETWORK_ERROR;
}
}

OverlayTranslationWorker::OverlayTranslationWorker(const QImage &image, const QVariantMap &params, std::shared_ptr<TranslationService> service, QObject *parent)
    : QThread(parent), image_(image), service_(std::move(service)) {
    target_lang_ = params.value("target_lang", "ZH").toString();
    // An absent key means the configured defaults, as in every other translation request.
    preserve_formatting_ = params.value("preserve_formatting", true).toBool();
    split_sentences_ = params.value("split_sentences", "nonewlines").toString();
    if (!service_) return;
    try {
        provider_ = service_->provider();
    } catch (const std::invalid_argument &e) {
        configuration_error_ = QString::fromUtf8(e.what());
        return;
    }
    if (provider_) quota_error_ = service_->check_quota(*provider_);
}

void OverlayTranslationWorker::abort() {
    cancelled_ = true;
    // Interrupts the request in flight instead of leaving the caller to wait out its
    // timeout, which a thinking model can make a minute long.
    cancellation_.cancel();
    requestInterruption();
}

void OverlayTranslationWorker::run() {
    // http_request watches this thread's cancellation, so a cancelled capture ends the
    // call at once.
    const HttpCancellationScope scope(&cancellation_);
    const auto fail = [this](const QString &reason) {
        error_ = reason;
        succeeded_ = false;
        log_warning(T("覆盖翻译失败: {reason}", {{"reason", reason}}), "OverlayTranslation");
        emit finished_signal(false);
    };
    try {
        if (!provider_) return fail(configuration_error_.isEmpty() ? overlay_tr("No translation service is selected.") : configuration_error_);
        if (!provider_->is_configured()) return fail(overlay_tr("The translation service is not configured yet."));
        if (quota_error_) return fail(quota_error_->error_message);
        emit progress_changed(0, 0);
        const auto recognition = OCRManager::instance().recognize(image_, &cancelled_);
        if (isInterruptionRequested()) return;
        if (recognition.code != 100) return fail(recognition.msg);
        blocks_ = group_overlay_blocks(recognition);
        if (blocks_.isEmpty()) return fail(overlay_tr("No text was recognized in the selection."));
        const int total = static_cast<int>(blocks_.size());
        log_info(T("覆盖翻译: 识别到 {total} 个段落", {{"total", total}}), "OverlayTranslation");
        emit progress_changed(0, total);

        // One failed batch switches the rest of the capture to per-paragraph calls: a model
        // that will not keep the array contract must not be tried again on every page.
        bool batch_usable = provider_->supports_batch();
        int done = 0;
        while (done < total) {
            if (isInterruptionRequested()) return;
            int end = done;
            int chars = 0;
            while (end < total && end - done < BATCH_MAX_TEXTS && (end == done || chars + blocks_[end].source.size() <= BATCH_MAX_CHARS)) {
                chars += blocks_[end].source.size();
                ++end;
            }
            if (batch_usable && end - done > 1) {
                QStringList texts;
                texts.reserve(end - done);
                for (int i = done; i < end; ++i) texts.append(blocks_[i].source);
                const TranslationRequest batch_request(QString(), target_lang_, std::nullopt, preserve_formatting_, 10, {{"split_sentences", split_sentences_}});
                const auto batch = provider_->translate_batch(texts, batch_request);
                if (isInterruptionRequested()) return;
                if (batch.success && batch.translations.size() == texts.size()) {
                    for (int i = done; i < end; ++i) {
                        blocks_[i].translated = batch.translations.at(i - done);
                        translated_chars_ += blocks_[i].source.toUcs4().size();
                    }
                    done = end;
                    emit progress_changed(done, total);
                    continue;
                }
                if (batch.error_code && fatal_batch_error(*batch.error_code)) return fail(batch.error_message);
                batch_usable = false;
                log_warning(T("批量翻译失败，回退逐段: {reason}", {{"reason", batch.error_message}}), "OverlayTranslation");
            }
            const TranslationRequest request(blocks_[done].source, target_lang_, std::nullopt, preserve_formatting_, 10, {{"split_sentences", split_sentences_}});
            const auto translated = provider_->translate(request);
            if (isInterruptionRequested()) return;
            if (!translated.success) return fail(translated.error_message);
            blocks_[done].translated = translated.translated_text;
            translated_chars_ += blocks_[done].source.toUcs4().size();
            ++done;
            emit progress_changed(done, total);
        }
        succeeded_ = true;
        emit finished_signal(true);
    } catch (const std::exception &e) {
        fail(QString::fromUtf8(e.what()));
    }
}
}
