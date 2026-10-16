#include "translation/providers/google.h"
#include "core/html_unescape.h"
#include "core/http_client.h"
#include "core/logger.h"
#include "translation/providers/common.h"
#include <QHash>

namespace jietuba {
GoogleTranslateProvider::GoogleTranslateProvider(const ProviderConfig &config) : api_key_(config.value("api_key").toString().trimmed()) {}

ProviderMetadata GoogleTranslateProvider::class_metadata() {
    ProviderMetadata metadata;
    metadata.provider_id = "google";
    metadata.display_name = "Google";
    metadata.credentials = {ProviderField::text("google_translate_api_key", "Google API Key", "Google API Key", true)};
    metadata.help_label = "Google Cloud Console";
    metadata.help_url = "https://console.cloud.google.com/apis/credentials";
    return metadata;
}

TranslationResult GoogleTranslateProvider::translate(const TranslationRequest &request) {
    if (request.text.trimmed().isEmpty()) return TranslationResult::failure(TranslationErrorCode::INVALID_REQUEST, "Text is empty");
    if (!is_configured()) return TranslationResult::failure(TranslationErrorCode::NOT_CONFIGURED, "Google Cloud Translation API key is not configured");

    QJsonObject body{{"q", request.text}, {"target", to_google_code(request.target_lang)}, {"format", "text"}};
    if (request.source_lang) body.insert("source", to_google_code(*request.source_lang));
    const auto url = API_URL + "?" + python_urlencode({{"key", api_key_}});
    const auto response = http_request("POST", url, {{"Content-Type", "application/json; charset=utf-8"}}, compact_json(body), request.timeout);
    if (response.kind == HttpResponse::HttpError) return http_error(response);
    if (response.kind == HttpResponse::Timeout) {
        log_error(QString("Google Translate timed out after %1s").arg(request.timeout), "GoogleTranslate");
        return TranslationResult::failure(TranslationErrorCode::NETWORK_ERROR, QString("Request timed out after %1s").arg(request.timeout));
    }
    if (response.kind == HttpResponse::NetworkError) {
        log_error("Google Translate network error: " + response.reason, "GoogleTranslate");
        return TranslationResult::failure(TranslationErrorCode::NETWORK_ERROR, "Network error: " + response.reason);
    }
    const auto raw = parse_json_body(response.body);
    if (!raw) {
        log_error(QString("Google Translate response error: invalid JSON"), "GoogleTranslate");
        return TranslationResult::failure(TranslationErrorCode::UNKNOWN, "Failed to parse Google Cloud Translation response");
    }
    const auto translations = raw->toObject().value("data").toObject().value("translations").toArray();
    if (translations.isEmpty()) return TranslationResult::failure(TranslationErrorCode::UNKNOWN, "Invalid Google Cloud Translation response");
    const auto translation = translations.first().toObject();
    const auto translated = html_unescape(json_text(translation.value("translatedText")));
    if (translated.isEmpty()) return TranslationResult::failure(TranslationErrorCode::UNKNOWN, "Invalid Google Cloud Translation response");
    const auto detected = normalize_language_code(translation.value("detectedSourceLanguage").toString());
    return TranslationResult{true, translated, detected.value_or(QString()), std::nullopt, {}};
}

QString GoogleTranslateProvider::to_google_code(const QString &language_code) {
    static const QHash<QString, QString> codes = {{"zh-Hans", "zh-CN"}, {"zh-Hant", "zh-TW"}, {"en-US", "en"}, {"en-GB", "en"}, {"pt-BR", "pt"}};
    return codes.value(language_code, language_code);
}

TranslationResult GoogleTranslateProvider::http_error(const HttpResponse &response) const {
    const auto details = error_body_object(response.body).value("error").toObject();
    const auto status = json_text(details.value("status"));
    const auto message = first_text({details.value("message"), QJsonValue(response.reason)}, QString("HTTP %1").arg(response.status));
    const auto code = map_error_code(status, message, response.status);
    // Never log the request URL because its query contains the API key.
    log_error(QString("Google Translate HTTP %1: %2").arg(response.status).arg(status.isEmpty() ? message : status), "GoogleTranslate");
    return TranslationResult::failure(code, message);
}

TranslationErrorCode GoogleTranslateProvider::map_error_code(const QString &status, const QString &message, int status_code) {
    const auto lowered = message.toLower();
    if (status == "RESOURCE_EXHAUSTED" || status_code == 429 || lowered.contains("quota") || lowered.contains("rate limit")) return TranslationErrorCode::RATE_LIMITED;
    if (status == "UNAUTHENTICATED" || status == "PERMISSION_DENIED" || status_code == 401 || status_code == 403) return TranslationErrorCode::AUTH_FAILED;
    if (status == "INVALID_ARGUMENT" || status_code == 400) {
        if (lowered.contains("language")) return TranslationErrorCode::UNSUPPORTED_LANGUAGE;
        return TranslationErrorCode::INVALID_REQUEST;
    }
    if (status_code >= 500) return TranslationErrorCode::NETWORK_ERROR;
    return TranslationErrorCode::UNKNOWN;
}
}
