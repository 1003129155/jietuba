#pragma once
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <optional>

namespace jietuba {
// Provider-neutral error classes.
enum class TranslationErrorCode { NOT_CONFIGURED, AUTH_FAILED, QUOTA_EXCEEDED, RATE_LIMITED, NETWORK_ERROR, UNSUPPORTED_LANGUAGE, INVALID_REQUEST, UNKNOWN };
QString translation_error_code_value(TranslationErrorCode code);

// Legacy and provider codes to the application's BCP-47 style codes; nullopt for empty or "auto".
std::optional<QString> normalize_language_code(const QString &code);

struct TranslationRequest {
    // target_lang and source_lang are normalized on construction.
    TranslationRequest(const QString &text, const QString &target_lang, const std::optional<QString> &source_lang = std::nullopt, bool preserve_formatting = true,
                       int timeout = 10, const QVariantMap &options = {});
    QString text;
    QString target_lang;
    std::optional<QString> source_lang;
    bool preserve_formatting = true;
    int timeout = 10;
    QVariantMap options;
};

struct TranslationResult {
    bool success = false;
    QString translated_text;
    QString detected_source_lang;
    std::optional<TranslationErrorCode> error_code;
    QString error_message;
    static TranslationResult failure(TranslationErrorCode code, const QString &message) { return TranslationResult{false, {}, {}, code, message}; }
};

// Several texts in one request. An LLM reply costs a round trip, and a capture's
// paragraphs are short: sending them together turns dozens of calls into a few. A
// failed batch is not fatal, since the caller can still translate text by text.
struct BatchTranslationResult {
    bool success = false;
    // Same size and order as the requested texts; empty along with success = false.
    QStringList translations;
    QString detected_source_lang;
    std::optional<TranslationErrorCode> error_code;
    QString error_message;
    static BatchTranslationResult failure(TranslationErrorCode code, const QString &message) {
        return BatchTranslationResult{false, {}, {}, code, message};
    }
    // The failure a single-text exchange reported, for a batch that went the same way.
    static BatchTranslationResult failure(const TranslationResult &single) {
        return failure(single.error_code.value_or(TranslationErrorCode::UNKNOWN), single.error_message);
    }
};
}

Q_DECLARE_METATYPE(jietuba::TranslationResult)
