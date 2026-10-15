#pragma once
#include "translation/provider.h"

namespace jietuba {
// DeepL behind the provider-neutral API.
class DeepLProvider : public TranslationProvider {
public:
    explicit DeepLProvider(const ProviderConfig &config);
    static ProviderMetadata class_metadata();
    QString provider_id() const override { return "deepl"; }
    QString display_name() const override { return "DeepL API"; }
    bool is_configured() const override { return !api_key_.isEmpty(); }
    TranslationResult translate(const TranslationRequest &request) override;
private:
    static QString to_deepl_code(const QString &language_code);
    static TranslationErrorCode error_code(const QString &message);
    QString api_key_;
    bool use_pro_;
};
}
