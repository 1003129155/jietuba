#include "translation/overlay_translation_worker.h"
#include "core/i18n.h"
#include "core/logger.h"
#include "translation/paragraph_translator.h"
#include <algorithm>
#include <stdexcept>

namespace jietuba {
namespace {
QString overlay_tr(const char *text) { return i18n_tr(text, "OverlayTranslation"); }
}

OverlayTranslationWorker::OverlayTranslationWorker(const QImage &image, const QVariantMap &params, std::shared_ptr<TranslationService> service, QObject *parent)
    : QThread(parent), image_(image), service_(std::move(service)) {
    qRegisterMetaType<QVector<OverlayTextBlock>>();
    target_lang_ = params.value("target_lang", "ZH").toString();
    // An absent key means the configured defaults, as in every other translation request.
    preserve_formatting_ = params.value("preserve_formatting", true).toBool();
    split_sentences_ = params.value("split_sentences", "nonewlines").toString();
    if (!service_) return;
    try {
        providers_.push_back(service_->provider());
        // One instance per request that may be in flight.
        const int parallel = std::max(1, providers_.front()->parallel_requests);
        while (static_cast<int>(providers_.size()) < parallel) providers_.push_back(service_->provider());
    } catch (const std::invalid_argument &e) {
        providers_.clear();
        configuration_error_ = QString::fromUtf8(e.what());
        return;
    }
    quota_error_ = service_->check_quota(*providers_.front());
}

QString OverlayTranslationWorker::failure_notice() const {
    if (failed_blocks_ <= 0) return {};
    const auto notice = overlay_tr("%1 paragraphs could not be translated and were left as they are.").replace("%1", QString::number(failed_blocks_));
    return failure_reason_.isEmpty() ? notice : notice + "\n" + failure_reason_;
}

void OverlayTranslationWorker::abort() {
    cancelled_ = true;
    // Interrupts the requests in flight instead of leaving the caller to wait out their
    // timeout, which a thinking model can make a minute long.
    cancellation_.cancel();
    requestInterruption();
}

void OverlayTranslationWorker::set_blocks(const QVector<OverlayTextBlock> &blocks) {
    blocks_ = blocks;
    recognized_ = true;
}

void OverlayTranslationWorker::run() {
    const auto fail = [this](const QString &reason) {
        error_ = reason;
        succeeded_ = false;
        log_warning(T("覆盖翻译失败: {reason}", {{"reason", reason}}), "OverlayTranslation");
        emit finished_signal(false);
    };
    try {
        if (providers_.empty()) return fail(configuration_error_.isEmpty() ? overlay_tr("No translation service is selected.") : configuration_error_);
        if (!providers_.front()->is_configured()) return fail(overlay_tr("The translation service is not configured yet."));
        if (quota_error_) return fail(quota_error_->error_message);
        emit progress_changed(0, 0);
        if (!recognized_) {
            const auto recognition = recognize_text(image_, &cancelled_);
            if (cancelled_) return;
            if (recognition.code != 100) return fail(recognition.msg.isEmpty() ? overlay_tr("Text recognition failed.") : recognition.msg);
            blocks_ = group_overlay_blocks(recognition);
            if (blocks_.isEmpty()) return fail(overlay_tr("No text was recognized in the selection."));
            log_info(T("覆盖翻译: 识别到 {total} 个段落", {{"total", static_cast<int>(blocks_.size())}}), "OverlayTranslation");
            emit recognized(blocks_);
        }
        // Only what has no translation yet is sent: a run after a partial failure asks for
        // the paragraphs that failed, not for the whole capture again.
        QVector<int> pending;
        QStringList texts;
        for (int index = 0; index < blocks_.size(); ++index) {
            if (!blocks_[index].translated.isEmpty()) continue;
            if (const auto known = known_.find(blocks_[index].source); known != known_.end()) {
                blocks_[index].translated = known.value();
                emit block_translated(index, known.value());
                continue;
            }
            pending.append(index);
            texts.append(blocks_[index].source);
        }
        const int total = static_cast<int>(pending.size());
        emit progress_changed(0, total);
        if (total > 0) {
            std::vector<TranslationProvider *> providers;
            for (const auto &provider : providers_) providers.push_back(provider.get());
            const TranslationRequest prototype(QString(), target_lang_, std::nullopt, preserve_formatting_, 10, {{"split_sentences", split_sentences_}});
            int done = 0;
            const auto translated = translate_paragraphs(texts, providers, prototype, &cancellation_, [&](int index, const QString &translation) {
                blocks_[pending[index]].translated = translation;
                emit block_translated(pending[index], translation);
                emit progress_changed(++done, total);
            });
            if (cancelled_ || translated.cancelled) return;
            translated_chars_ = translated.translated_chars;
            // Nothing came back at all: that is a failure to report, not a capture to leave as it is.
            if (translated.fatal || translated.failed >= total)
                return fail(translated.error.isEmpty() ? overlay_tr("The translation service returned no text.") : translated.error);
            failed_blocks_ = translated.failed;
            failure_reason_ = translated.error;
            if (failed_blocks_ > 0)
                log_warning(T("覆盖翻译: {count} 个段落未译出: {reason}", {{"count", failed_blocks_}, {"reason", failure_reason_}}), "OverlayTranslation");
        }
        // Drawn here, not on the GUI thread: a long screenshot has hundreds of blocks to fit.
        if (paint_result_) result_ = paint_translations(image_, blocks_, target_lang_, &painted_blocks_, &unplaced_);
        if (cancelled_) return;
        succeeded_ = true;
        emit finished_signal(true);
    } catch (const std::exception &e) {
        fail(QString::fromUtf8(e.what()));
    }
}
}
