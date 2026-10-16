#pragma once
#include "translation/provider.h"

namespace jietuba {
struct HttpResponse;

// Microsoft Azure Translator (Cognitive Services) REST API v3.0.
class AzureTranslateProvider : public TranslationProvider {
public:
    static inline const QString API_URL = "https://api.cognitive.microsofttranslator.com/translate";
    static inline const QString API_VERSION = "3.0";
    static constexpr int MAX_TEXT_LENGTH = 50000;
    explicit AzureTranslateProvider(const ProviderConfig &config);
    static ProviderMetadata class_metadata();
    QString provider_id() const override { return "azure"; }
    QString display_name() const override { return "Azure"; }
    bool is_configured() const override { return !api_key_.isEmpty(); }
    TranslationResult translate(const TranslationRequest &request) override;
private:
    static QString to_azure_code(const QString &language_code);
    TranslationResult http_error(const HttpResponse &response) const;
    static TranslationErrorCode map_error_code(const QString &err_code, int status_code);
    QString api_key_;
    QString region_;
    QString endpoint_;
};
}
