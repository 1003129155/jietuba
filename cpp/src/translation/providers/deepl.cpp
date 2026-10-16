#include "translation/providers/deepl.h"
#include "translation/deepl_service.h"
#include <QHash>

namespace jietuba {
DeepLProvider::DeepLProvider(const ProviderConfig &config) : api_key_(config.value("api_key").toString()), use_pro_(config.value("use_pro", false).toBool()) {}

ProviderMetadata DeepLProvider::class_metadata() {
    ProviderMetadata metadata;
    metadata.provider_id = "deepl";
    metadata.display_name = "DeepL API";
    metadata.credentials = {ProviderField::text("deepl_api_key", "DeepL API Key", "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx:fx", true)};
    metadata.options = {ProviderField::toggle("deepl_use_pro", "Use DeepL Pro API", "Enable if you have a paid DeepL subscription")};
    // Only DeepL's API takes these two; this only drives the UI, requests always carry them.
    metadata.supported_request_options = {"split_sentences", "preserve_formatting"};
    metadata.notice = "DeepL free tier: 500,000 chars/month. Get API key at";
    metadata.help_label = "deepl.com/pro-api";
    metadata.help_url = "https://www.deepl.com/pro-api";
    return metadata;
}

TranslationResult DeepLProvider::translate(const TranslationRequest &request) {
    const auto split_sentences = request.options.value("split_sentences", "nonewlines").toString();
    DeepLService service(api_key_, use_pro_);
    const auto raw = service.translate(request.text, to_deepl_code(request.target_lang), request.source_lang ? std::optional<QString>(to_deepl_code(*request.source_lang)) : std::nullopt,
                                       split_sentences, request.preserve_formatting, request.timeout);
    if (raw.success) {
        const auto detected = normalize_language_code(raw.detected_source_lang);
        return TranslationResult{true, raw.translated_text, detected.value_or(QString()), std::nullopt, {}};
    }
    const auto message = raw.error.isEmpty() ? QString("Translation failed") : raw.error;
    return TranslationResult::failure(error_code(message), message);
}

QString DeepLProvider::to_deepl_code(const QString &language_code) {
    static const QHash<QString, QString> codes = {
        {"zh-Hans", "ZH"}, {"zh-Hant", "ZH-HANT"}, {"en", "EN"}, {"en-US", "EN-US"}, {"en-GB", "EN-GB"}, {"ja", "JA"}, {"ko", "KO"},
        {"vi", "VI"}, {"de", "DE"}, {"fr", "FR"}, {"es", "ES"}, {"it", "IT"}, {"pt", "PT"}, {"pt-BR", "PT-BR"}, {"pt-PT", "PT-PT"},
        {"ru", "RU"}, {"pl", "PL"}, {"nl", "NL"}, {"tr", "TR"}, {"th", "TH"}, {"ar", "AR"}, {"id", "ID"}, {"uk", "UK"},
    };
    return codes.value(language_code, language_code.toUpper());
}

TranslationErrorCode DeepLProvider::error_code(const QString &message) {
    const auto lowered = message.toLower();
    if (lowered.contains("api key") || lowered.contains("permission")) return TranslationErrorCode::AUTH_FAILED;
    if (lowered.contains("quota")) return TranslationErrorCode::QUOTA_EXCEEDED;
    if (lowered.contains("too many requests")) return TranslationErrorCode::RATE_LIMITED;
    if (lowered.contains("network")) return TranslationErrorCode::NETWORK_ERROR;
    if (lowered.contains("empty") || lowered.contains("bad request")) return TranslationErrorCode::INVALID_REQUEST;
    return TranslationErrorCode::UNKNOWN;
}
}
