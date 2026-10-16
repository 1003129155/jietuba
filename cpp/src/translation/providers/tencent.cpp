#include "translation/providers/tencent.h"
#include "core/logger.h"
#include "translation/providers/common.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QHash>
#include <QMessageAuthenticationCode>
#include <QTimeZone>

namespace jietuba {
namespace {
QByteArray hmac_sha256(const QByteArray &key, const QString &message) {
    return QMessageAuthenticationCode::hash(message.toUtf8(), key, QCryptographicHash::Sha256);
}
QString sha256_hex(const QByteArray &data) { return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex()); }
const QString INVALID_RESPONSE = "Invalid Tencent MT response";
// Target languages Tencent does not offer.
const QSet<QString> UNSUPPORTED_TARGET = {"nl", "pl", "uk"};
}

TencentTranslateProvider::TencentTranslateProvider(const ProviderConfig &config)
    : secret_id_(config.value("secret_id").toString().trimmed()), secret_key_(config.value("secret_key").toString().trimmed()) {
    min_timeout = 5;
    quota_limit_chars = QUOTA_LIMIT_CHARS;
}

ProviderMetadata TencentTranslateProvider::class_metadata() {
    ProviderMetadata metadata;
    metadata.provider_id = "tencent";
    metadata.display_name = "Tencent";
    metadata.credentials = {
        ProviderField::text("tencent_translate_secret_id", "SecretId", "Tencent Cloud API SecretId (Console > Access Management > API Keys)"),
        ProviderField::text("tencent_translate_secret_key", "SecretKey", "Tencent Cloud API SecretKey, paired with the SecretId", true),
    };
    metadata.options = {
        ProviderField::toggle("tencent_translate_quota_enabled", "Enable monthly quota guard", "Stop translation once the monthly free quota is reached"),
        ProviderField::text("tencent_translate_quota_limit", "Monthly quota (characters)", "4990000"),
    };
    // The API refuses overuse with FailedOperation.NoFreeAmount until the account is
    // upgraded to paid; the local guard is an extra safeguard.
    metadata.notice = "Tencent Cloud TMT: about 5M free characters/month. The API rejects overuse itself; the local quota guard (default on) is extra protection.";
    metadata.help_label = "Tencent Cloud Console";
    metadata.help_url = "https://console.cloud.tencent.com/cam/capi";
    return metadata;
}

TranslationResult TencentTranslateProvider::translate(const TranslationRequest &request) {
    if (request.text.trimmed().isEmpty()) return TranslationResult::failure(TranslationErrorCode::INVALID_REQUEST, "Text is empty");
    if (!is_configured()) return TranslationResult::failure(TranslationErrorCode::NOT_CONFIGURED, "Tencent SecretId/SecretKey is not configured");
    if (UNSUPPORTED_TARGET.contains(request.target_lang))
        return TranslationResult::failure(TranslationErrorCode::UNSUPPORTED_LANGUAGE, "Tencent does not support target language " + request.target_lang);

    const auto payload = request_payload(request);
    // The HTTP stack always sends Host for the URL, which is the signed value.
    HttpHeaders headers;
    for (const auto &header : signed_headers(payload, QDateTime::currentSecsSinceEpoch()))
        if (header.first != "Host") headers.push_back(header);
    return handle_response(http_request("POST", request_url(), headers, payload, request.timeout), request.timeout);
}

QByteArray TencentTranslateProvider::request_payload(const TranslationRequest &request) const {
    return compact_json(QJsonObject{
        {"SourceText", request.text},
        {"Source", request.source_lang ? to_code(*request.source_lang) : QString("auto")},
        {"Target", to_code(request.target_lang)},
        {"ProjectId", PROJECT_ID},
    });
}

HttpHeaders TencentTranslateProvider::signed_headers(const QByteArray &payload, qint64 timestamp) const {
    // content-type is signed and must match what is sent, charset included.
    const QString content_type = "application/json; charset=utf-8";
    const auto hashed_payload = sha256_hex(payload);
    const auto canonical_headers = "content-type:" + content_type + "\nhost:" + HOST + "\nx-tc-action:" + ACTION.toLower() + "\n";
    const QString signed_header_names = "content-type;host;x-tc-action";
    const auto canonical_request = QStringList{"POST", "/", "", canonical_headers, signed_header_names, hashed_payload}.join("\n");

    // The date comes from the timestamp in UTC; local time is a day ahead after midnight in UTC+8.
    const auto date = QDateTime::fromSecsSinceEpoch(timestamp, QTimeZone::UTC).toString("yyyy-MM-dd");
    const auto credential_scope = date + "/" + SERVICE + "/tc3_request";
    const auto timestamp_text = QString::number(timestamp);
    const auto string_to_sign = QStringList{"TC3-HMAC-SHA256", timestamp_text, credential_scope, sha256_hex(canonical_request.toUtf8())}.join("\n");
    const auto secret_date = hmac_sha256(("TC3" + secret_key_).toUtf8(), date);
    const auto secret_service = hmac_sha256(secret_date, SERVICE);
    const auto secret_signing = hmac_sha256(secret_service, "tc3_request");
    const auto signature = QString::fromLatin1(hmac_sha256(secret_signing, string_to_sign).toHex());

    return {
        {"Authorization", "TC3-HMAC-SHA256 Credential=" + secret_id_ + "/" + credential_scope + ", SignedHeaders=" + signed_header_names + ", Signature=" + signature},
        {"Content-Type", content_type},
        {"Host", HOST},
        {"X-TC-Action", ACTION},
        {"X-TC-Region", REGION},
        {"X-TC-Timestamp", timestamp_text},
        {"X-TC-Version", VERSION},
    };
}

TranslationResult TencentTranslateProvider::handle_response(const HttpResponse &response, int timeout) const {
    if (response.kind == HttpResponse::HttpError) return http_error(response);
    if (response.kind == HttpResponse::Timeout) {
        log_error(QString("Tencent MT timed out after %1s").arg(timeout), "TencentTranslate");
        return TranslationResult::failure(TranslationErrorCode::NETWORK_ERROR, QString("Request timed out after %1s").arg(timeout));
    }
    if (response.kind == HttpResponse::NetworkError) {
        log_error("Tencent MT network error: " + response.reason, "TencentTranslate");
        return TranslationResult::failure(TranslationErrorCode::NETWORK_ERROR, "Network error: " + response.reason);
    }
    const auto raw = parse_json_body(response.body);
    if (!raw) {
        log_error(QString("Tencent MT response error: invalid JSON"), "TencentTranslate");
        return TranslationResult::failure(TranslationErrorCode::UNKNOWN, "Failed to parse Tencent MT response");
    }
    return parse_response(*raw);
}

TranslationResult TencentTranslateProvider::parse_response(const QJsonValue &raw) const {
    const auto response_value = raw.toObject().value("Response");
    if (!raw.isObject() || !response_value.isObject()) return TranslationResult::failure(TranslationErrorCode::UNKNOWN, INVALID_RESPONSE);
    const auto response = response_value.toObject();
    if (const auto error = response.value("Error"); error.isObject()) {
        const auto code = first_text({error.toObject().value("Code")});
        return TranslationResult::failure(map_api_error_code(code), first_text({error.toObject().value("Message")}, code));
    }
    const auto translated = first_text({response.value("TargetText")});
    if (translated.isEmpty()) return TranslationResult::failure(TranslationErrorCode::UNKNOWN, INVALID_RESPONSE);
    const auto detected = from_code(first_text({response.value("Source")}));
    return TranslationResult{true, translated, detected, std::nullopt, {}};
}

QString TencentTranslateProvider::to_code(const QString &language_code) {
    static const QHash<QString, QString> codes = {{"zh-Hans", "zh"}, {"zh-Hant", "zh-TW"}, {"en-US", "en"}, {"en-GB", "en"}, {"pt-BR", "pt"}, {"pt-PT", "pt"}};
    return codes.value(language_code, language_code);
}

QString TencentTranslateProvider::from_code(const QString &code) {
    static const QHash<QString, QString> codes = {{"zh", "zh-Hans"}, {"zh-TW", "zh-Hant"}};
    if (code.isEmpty()) return {};
    return codes.value(code, code);
}

TranslationResult TencentTranslateProvider::http_error(const HttpResponse &response) const {
    const auto data = error_body_object(response.body);
    if (data.value("Response").toObject().value("Error").isObject()) return parse_response(data);
    const auto message = response.reason.isEmpty() ? QString("HTTP %1").arg(response.status) : response.reason;
    const auto code = response.status == 401 || response.status == 403 ? TranslationErrorCode::AUTH_FAILED
                      : response.status >= 500                          ? TranslationErrorCode::NETWORK_ERROR
                                                                        : TranslationErrorCode::UNKNOWN;
    log_error(QString("Tencent MT HTTP %1: %2").arg(response.status).arg(message), "TencentTranslate");
    return TranslationResult::failure(code, message);
}

TranslationErrorCode TencentTranslateProvider::map_api_error_code(const QString &code) {
    // From the TextTranslate documentation and Tencent Cloud's common error codes.
    const auto contains_any = [&code](std::initializer_list<const char *> parts) {
        for (const char *part : parts)
            if (code.contains(QLatin1String(part))) return true;
        return false;
    };
    if (contains_any({"AuthFailure", "UnauthorizedOperation"})) return TranslationErrorCode::AUTH_FAILED;
    if (code.contains("NoFreeAmount")) return TranslationErrorCode::QUOTA_EXCEEDED;
    if (code == "FailedOperation.ServiceIsolate" || code == "FailedOperation.StopUsing") return TranslationErrorCode::QUOTA_EXCEEDED;
    if (code == "FailedOperation.UserNotRegistered") return TranslationErrorCode::AUTH_FAILED;
    if (contains_any({"UnSupportedTargetLanguage", "UnsupportedLanguage", "UnsupportedSourceLanguage"})) return TranslationErrorCode::UNSUPPORTED_LANGUAGE;
    if (code.contains("TextTooLong")) return TranslationErrorCode::INVALID_REQUEST;
    if (contains_any({"LimitedAccessFrequency", "RequestLimitExceeded", "LimitExceeded"})) return TranslationErrorCode::RATE_LIMITED;
    if (contains_any({"InvalidParameter", "MissingParameter"})) return TranslationErrorCode::INVALID_REQUEST;
    if (code.contains("InternalError")) return TranslationErrorCode::NETWORK_ERROR;
    return TranslationErrorCode::UNKNOWN;
}
}
