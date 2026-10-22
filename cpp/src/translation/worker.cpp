#include "translation/worker.h"
#include "core/logger.h"
#include <stdexcept>

namespace jietuba {
TranslationWorker::TranslationWorker(const TranslationService &service, const TranslationRequest &request, const QString &provider_id, const QVariantMap &provider_overrides,
                                     QObject *parent)
    : QThread(parent), service_(service), request_(request) {
    qRegisterMetaType<jietuba::TranslationResult>();
    try {
        provider_ = service.provider(provider_id, provider_overrides);
    } catch (const std::invalid_argument &e) {
        configuration_error_ = QString::fromUtf8(e.what());
    }
    if (provider_) quota_error_ = service.check_quota(*provider_);
    connect(this, &TranslationWorker::usage_signal, this, [this](qint64 chars) { service_.record_usage(*provider_, chars); }, Qt::QueuedConnection);
}

int TranslationWorker::effective_timeout_ms() const { return provider_ ? provider_->effective_timeout(request_) * 1000 : 0; }

void TranslationWorker::abort() {
    requestInterruption();
    cancellation_.cancel();
}

void TranslationWorker::run() {
    const HttpCancellationScope scope(&cancellation_);
    TranslationResult result;
    try {
        if (!provider_) result = TranslationResult::failure(TranslationErrorCode::NOT_CONFIGURED, configuration_error_);
        else if (!provider_->is_configured()) result = TranslationResult::failure(TranslationErrorCode::NOT_CONFIGURED, provider_->display_name() + " is not configured");
        else if (quota_error_) result = *quota_error_;
        else {
            result = provider_->translate(request_);
            if (result.success) emit usage_signal(request_.text.toUcs4().size());
        }
    } catch (const std::exception &e) {
        log_error(T("翻译线程异常: {exc}", {{"exc", QString::fromUtf8(e.what())}}), "Translation");
        result = TranslationResult::failure(TranslationErrorCode::UNKNOWN, QString("Translation failed: %1").arg(QString::fromUtf8(e.what())));
    }
    if (!isInterruptionRequested()) emit finished_signal(result);
}
}
