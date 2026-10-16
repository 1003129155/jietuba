#pragma once
#include "core/http_client.h"
#include "translation/service.h"
#include <QThread>

namespace jietuba {
// The Qt worker shared by every provider. Configuration resolves on the GUI thread;
// the worker thread only runs the provider's synchronous network call and never touches QSettings.
class TranslationWorker : public QThread {
    Q_OBJECT
public:
    TranslationWorker(const TranslationService &service, const TranslationRequest &request, const QString &provider_id = {}, const QVariantMap &provider_overrides = {},
                      QObject *parent = nullptr);
    int effective_timeout_ms() const;
    // Interrupts the thread and aborts its network request, so it ends promptly
    // instead of at the provider's timeout; no result is emitted.
    void abort();
signals:
    void finished_signal(const jietuba::TranslationResult &result);
    // Emitted from run() and queued to this object's (GUI) thread for the bookkeeping.
    void usage_signal(qint64 chars);
protected:
    void run() override;
private:
    TranslationService service_;
    TranslationRequest request_;
    std::unique_ptr<TranslationProvider> provider_;
    QString configuration_error_;
    std::optional<TranslationResult> quota_error_;
    HttpCancellation cancellation_;
};
}
