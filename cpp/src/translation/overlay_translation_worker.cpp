#include "translation/overlay_translation_worker.h"
#include "core/i18n.h"
#include "core/logger.h"
#include "ocr/ocr_manager.h"
#include <stdexcept>

namespace jietuba {
namespace {
QString overlay_tr(const char *text) { return i18n_tr(text, "OverlayTranslation"); }
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
    requestInterruption();
}

void OverlayTranslationWorker::run() {
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
        for (int index = 0; index < total; ++index) {
            if (isInterruptionRequested()) return;
            const TranslationRequest request(blocks_[index].source, target_lang_, std::nullopt, preserve_formatting_, 10,
                                             {{"split_sentences", split_sentences_}});
            const auto translated = provider_->translate(request);
            if (isInterruptionRequested()) return;
            if (!translated.success) return fail(translated.error_message);
            blocks_[index].translated = translated.translated_text;
            translated_chars_ += blocks_[index].source.toUcs4().size();
            emit progress_changed(index + 1, total);
        }
        succeeded_ = true;
        emit finished_signal(true);
    } catch (const std::exception &e) {
        fail(QString::fromUtf8(e.what()));
    }
}
}
