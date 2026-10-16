#pragma once
#include "translation/provider.h"

namespace jietuba {
struct HttpResponse;

// Google NMT translation through the API-key-compatible v2 REST API.
class GoogleTranslateProvider : public TranslationProvider {
public:
    static inline const QString API_URL = "https://translation.googleapis.com/language/translate/v2";
    explicit GoogleTranslateProvider(const ProviderConfig &config);
    static ProviderMetadata class_metadata();
    QString provider_id() const override { return "google"; }
    QString display_name() const override { return "Google"; }
    bool is_configured() const override { return !api_key_.isEmpty(); }
    TranslationResult translate(const TranslationRequest &request) override;
private:
    static QString to_google_code(const QString &language_code);
    TranslationResult http_error(const HttpResponse &response) const;
    static TranslationErrorCode map_error_code(const QString &status, const QString &message, int status_code);
    QString api_key_;
};
}
