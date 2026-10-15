#include "translation/providers/azure.h"
#include "core/http_client.h"
#include "core/logger.h"
#include "translation/providers/common.h"
#include <QHash>
#include <QUuid>

namespace jietuba {
AzureTranslateProvider::AzureTranslateProvider(const ProviderConfig &config)
    : api_key_(config.value("api_key").toString().trimmed()), region_(config.value("region").toString().trimmed()) {
    endpoint_ = config.value("endpoint").toString().trimmed();
    while (endpoint_.endsWith('/')) endpoint_.chop(1);
}

ProviderMetadata AzureTranslateProvider::class_metadata() {
    ProviderMetadata metadata;
    metadata.provider_id = "azure";
    metadata.display_name = "Azure";
    metadata.credentials = {
        ProviderField::text("azure_translate_api_key", "Azure API Key", "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", true),
        ProviderField::text("azure_translate_region", "Azure Region", "eastasia"),
        ProviderField::text("azure_translate_endpoint", "Azure Endpoint", "Optional, use default if empty"),
    };
    metadata.help_label = "portal.azure.com";
    metadata.help_url = "https://portal.azure.com/";
    return metadata;
}

TranslationResult AzureTranslateProvider::translate(const TranslationRequest &request) {
    if (request.text.trimmed().isEmpty()) return TranslationResult::failure(TranslationErrorCode::INVALID_REQUEST, "Text is empty");
    if (request.text.toUcs4().size() > MAX_TEXT_LENGTH)
        return TranslationResult::failure(TranslationErrorCode::INVALID_REQUEST, "Text exceeds Azure Translator's 50,000-character limit");
    if (!is_configured()) return TranslationResult::failure(TranslationErrorCode::NOT_CONFIGURED, "Azure Translator API key is not configured");

    const auto base_url = endpoint_.isEmpty() ? API_URL : endpoint_ + "/translate";
    std::vector<std::pair<QString, QString>> params = {{"api-version", API_VERSION}, {"to", to_azure_code(request.target_lang)}};
    if (request.source_lang) params.emplace_back("from", to_azure_code(*request.source_lang));
    const auto url = base_url + "?" + python_urlencode(params);
    const auto body = compact_json(QJsonArray{QJsonObject{{"Text", request.text}}});
    HttpHeaders headers = {
        {"Content-Type", "application/json; charset=utf-8"},
        {"Ocp-Apim-Subscription-Key", api_key_},
        {"X-ClientTraceId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
    };
    if (!region_.isEmpty()) headers.emplace_back("Ocp-Apim-Subscription-Region", region_);

    const auto response = http_request("POST", url, headers, body, request.timeout);
    if (response.kind == HttpResponse::HttpError) return http_error(response);
    if (response.kind == HttpResponse::Timeout) {
        log_error(QString("Azure Translate timed out after %1s").arg(request.timeout), "AzureTranslate");
        return TranslationResult::failure(TranslationErrorCode::NETWORK_ERROR, QString("Request timed out after %1s").arg(request.timeout));
    }
    if (response.kind == HttpResponse::NetworkError) {
        log_error("Azure Translate network error: " + response.reason, "AzureTranslate");
        return TranslationResult::failure(TranslationErrorCode::NETWORK_ERROR, "Network error: " + response.reason);
    }
    const auto raw = parse_json_body(response.body);
    if (!raw) {
        log_error(QString("Azure Translate response error: invalid JSON"), "AzureTranslate");
        return TranslationResult::failure(TranslationErrorCode::UNKNOWN, "Failed to parse Azure Translator response");
    }
    if (!raw->isArray() || raw->toArray().isEmpty()) return TranslationResult::failure(TranslationErrorCode::UNKNOWN, "Invalid Azure Translator response");
    const auto first = raw->toArray().first().toObject();
    const auto translations = first.value("translations").toArray();
    if (translations.isEmpty()) return TranslationResult::failure(TranslationErrorCode::UNKNOWN, "Invalid Azure Translator response");
    const auto translated = json_text(translations.first().toObject().value("text"));
    if (translated.isEmpty()) return TranslationResult::failure(TranslationErrorCode::UNKNOWN, "Invalid Azure Translator response");
    QString detected;
    const auto detected_lang = first.value("detectedLanguage").toObject();
    if (!detected_lang.isEmpty()) detected = normalize_language_code(detected_lang.value("language").toString()).value_or(QString());
    return TranslationResult{true, translated, detected, std::nullopt, {}};
}

QString AzureTranslateProvider::to_azure_code(const QString &language_code) {
    static const QHash<QString, QString> codes = {{"zh-Hans", "zh-Hans"}, {"zh-Hant", "zh-Hant"}, {"en-US", "en"}, {"en-GB", "en"}, {"pt-BR", "pt"}};
    return codes.value(language_code, language_code);
}

TranslationResult AzureTranslateProvider::http_error(const HttpResponse &response) const {
    // Azure returns {"error":{"code":"...", "message":"..."}}
    const auto err_obj = error_body_object(response.body).value("error").toObject();
    const auto err_code = json_text(err_obj.value("code"));
    const auto message = first_text({err_obj.value("message"), QJsonValue(response.reason)}, QString("HTTP %1").arg(response.status));
    const auto code = map_error_code(err_code, response.status);
    log_error(QString("Azure Translate HTTP %1: %2").arg(response.status).arg(err_code.isEmpty() ? message : err_code), "AzureTranslate");
    return TranslationResult::failure(code, message);
}

TranslationErrorCode AzureTranslateProvider::map_error_code(const QString &err_code, int status_code) {
    const auto code_lower = err_code.toLower();
    if (status_code == 401 || status_code == 403 || code_lower.contains("unauthorized")) return TranslationErrorCode::AUTH_FAILED;
    if (status_code == 429 || code_lower.contains("ratelimit")) return TranslationErrorCode::RATE_LIMITED;
    if (status_code == 400) {
        if (code_lower.contains("language")) return TranslationErrorCode::UNSUPPORTED_LANGUAGE;
        return TranslationErrorCode::INVALID_REQUEST;
    }
    if (status_code >= 500) return TranslationErrorCode::NETWORK_ERROR;
    return TranslationErrorCode::UNKNOWN;
}
}
