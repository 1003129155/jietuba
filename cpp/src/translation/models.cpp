#include "translation/models.h"
#include <QHash>

namespace jietuba {
QString translation_error_code_value(TranslationErrorCode code) {
    switch (code) {
    case TranslationErrorCode::NOT_CONFIGURED: return "not_configured";
    case TranslationErrorCode::AUTH_FAILED: return "auth_failed";
    case TranslationErrorCode::QUOTA_EXCEEDED: return "quota_exceeded";
    case TranslationErrorCode::RATE_LIMITED: return "rate_limited";
    case TranslationErrorCode::NETWORK_ERROR: return "network_error";
    case TranslationErrorCode::UNSUPPORTED_LANGUAGE: return "unsupported_language";
    case TranslationErrorCode::INVALID_REQUEST: return "invalid_request";
    case TranslationErrorCode::UNKNOWN: return "unknown";
    }
    return "unknown";
}

std::optional<QString> normalize_language_code(const QString &code) {
    static const QHash<QString, QString> legacy = {
        {"ZH", "zh-Hans"}, {"ZH-HANS", "zh-Hans"}, {"ZH-HANT", "zh-Hant"}, {"EN", "en"}, {"EN-US", "en-US"}, {"EN-GB", "en-GB"},
        {"JA", "ja"}, {"KO", "ko"}, {"VI", "vi"}, {"DE", "de"}, {"FR", "fr"}, {"ES", "es"}, {"IT", "it"}, {"PT", "pt"},
        {"PT-BR", "pt-BR"}, {"PT-PT", "pt-PT"}, {"RU", "ru"}, {"PL", "pl"}, {"NL", "nl"}, {"TR", "tr"}, {"TH", "th"},
        {"AR", "ar"}, {"ID", "id"}, {"UK", "uk"},
    };
    if (code.isEmpty() || code.toLower() == "auto") return std::nullopt;
    return legacy.value(code.toUpper(), code);
}

TranslationRequest::TranslationRequest(const QString &text, const QString &target_lang, const std::optional<QString> &source_lang, bool preserve_formatting,
                                       int timeout, const QVariantMap &options)
    : text(text), target_lang(normalize_language_code(target_lang).value_or("en")), source_lang(source_lang ? normalize_language_code(*source_lang) : std::nullopt),
      preserve_formatting(preserve_formatting), timeout(timeout), options(options) {}
}
