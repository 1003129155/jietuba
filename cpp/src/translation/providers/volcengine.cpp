#include "translation/providers/volcengine.h"
#include "core/logger.h"
#include "translation/providers/common.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QHash>
#include <QMessageAuthenticationCode>

namespace jietuba {
namespace {
QByteArray hmac_sha256(const QByteArray &key, const QString &message) {
    return QMessageAuthenticationCode::hash(message.toUtf8(), key, QCryptographicHash::Sha256);
}
QString sha256_hex(const QByteArray &data) { return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex()); }
const QString INVALID_RESPONSE = "Invalid Volcengine MT response";
}

VolcengineTranslateProvider::VolcengineTranslateProvider(const ProviderConfig &config)
    : access_key_id_(config.value("access_key_id").toString().trimmed()), secret_access_key_(config.value("secret_access_key").toString().trimmed()) {
    min_timeout = 5;
    quota_limit_chars = QUOTA_LIMIT_CHARS;
}

ProviderMetadata VolcengineTranslateProvider::class_metadata() {
    ProviderMetadata metadata;
    metadata.provider_id = "volcengine";
    metadata.display_name = "Volcengine";
    metadata.credentials = {
        ProviderField::text("volcengine_translate_access_key", "Access Key ID", "Volcengine AccessKey AK (Console > Access Control > Key Management)"),
        ProviderField::text("volcengine_translate_secret_key", "Secret Access Key", "Volcengine AccessKey SK, paired with the AK", true),
    };
    metadata.options = {
        ProviderField::toggle("volcengine_translate_quota_enabled", "Enable monthly quota guard", "Stop translation once the monthly free quota is reached"),
        ProviderField::text("volcengine_translate_quota_limit", "Monthly quota (characters)", "1990000"),
    };
    // Overuse is billed rather than refused, so the guard is on by default.
    metadata.notice = "Volcengine machine translation: about 2M free characters/month. Overuse is billed at pay-as-you-go rates, so a local quota guard "
                      "(default on) prevents surprise charges.";
    metadata.help_label = "Volcengine Console";
    metadata.help_url = "https://console.volcengine.com/iam/keymanage/";
    return metadata;
}

TranslationResult VolcengineTranslateProvider::translate(const TranslationRequest &request) {
    if (request.text.trimmed().isEmpty()) return TranslationResult::failure(TranslationErrorCode::INVALID_REQUEST, "Text is empty");
    if (!is_configured()) return TranslationResult::failure(TranslationErrorCode::NOT_CONFIGURED, "Volcengine AccessKey/SecretKey is not configured");

    const auto payload = request_payload(request);
    const auto x_date = QDateTime::currentDateTimeUtc().toString("yyyyMMdd'T'HHmmss'Z'");
    // The HTTP stack always sends Host for the URL, which is the signed value.
    HttpHeaders headers;
    for (const auto &header : signed_headers(payload, x_date))
        if (header.first != "Host") headers.push_back(header);
    return handle_response(http_request("POST", request_url(), headers, payload, request.timeout), request.timeout);
}

QByteArray VolcengineTranslateProvider::request_payload(const TranslationRequest &request) const {
    return compact_json(QJsonObject{
        {"SourceLanguage", request.source_lang ? to_code(*request.source_lang) : QString()},
        {"TargetLanguage", to_code(request.target_lang)},
        {"TextList", QJsonArray{request.text}},
    });
}

HttpHeaders VolcengineTranslateProvider::signed_headers(const QByteArray &payload, const QString &x_date) const {
    const auto hashed_payload = sha256_hex(payload);
    const auto query = "Action=" + ACTION + "&Version=" + VERSION;
    const auto short_date = x_date.left(8);
    const QString signed_header_names = "content-type;host;x-content-sha256;x-date";
    const auto canonical_headers = "content-type:application/json\nhost:" + HOST + "\nx-content-sha256:" + hashed_payload + "\nx-date:" + x_date.trimmed() + "\n";
    const auto canonical_request = QStringList{"POST", "/", query, canonical_headers, signed_header_names, hashed_payload}.join("\n");

    const auto credential_scope = short_date + "/" + REGION + "/" + SERVICE + "/request";
    const auto string_to_sign = QStringList{"HMAC-SHA256", x_date, credential_scope, sha256_hex(canonical_request.toUtf8())}.join("\n");
    const auto k_date = hmac_sha256(secret_access_key_.toUtf8(), short_date);
    const auto k_region = hmac_sha256(k_date, REGION);
    const auto k_service = hmac_sha256(k_region, SERVICE);
    const auto signing_key = hmac_sha256(k_service, "request");
    const auto signature = QString::fromLatin1(hmac_sha256(signing_key, string_to_sign).toHex());

    return {
        {"Authorization", "HMAC-SHA256 Credential=" + access_key_id_ + "/" + credential_scope + ", SignedHeaders=" + signed_header_names + ", Signature=" + signature},
        {"Content-Type", "application/json"},
        {"Host", HOST},
        {"X-Content-Sha256", hashed_payload},
        {"X-Date", x_date},
    };
}

TranslationResult VolcengineTranslateProvider::handle_response(const HttpResponse &response, int timeout) const {
    if (response.kind == HttpResponse::HttpError) return http_error(response);
    if (response.kind == HttpResponse::Timeout) {
        log_error(QString("Volcengine MT timed out after %1s").arg(timeout), "VolcengineTranslate");
        return TranslationResult::failure(TranslationErrorCode::NETWORK_ERROR, QString("Request timed out after %1s").arg(timeout));
    }
    if (response.kind == HttpResponse::NetworkError) {
        log_error("Volcengine MT network error: " + response.reason, "VolcengineTranslate");
        return TranslationResult::failure(TranslationErrorCode::NETWORK_ERROR, "Network error: " + response.reason);
    }
    const auto raw = parse_json_body(response.body);
    if (!raw) {
        log_error(QString("Volcengine MT response error: invalid JSON"), "VolcengineTranslate");
        return TranslationResult::failure(TranslationErrorCode::UNKNOWN, "Failed to parse Volcengine MT response");
    }
    return parse_response(*raw);
}

TranslationResult VolcengineTranslateProvider::parse_response(const QJsonValue &raw_value) const {
    if (!raw_value.isObject()) return TranslationResult::failure(TranslationErrorCode::UNKNOWN, INVALID_RESPONSE);
    const auto raw = raw_value.toObject();
    const auto meta = raw.value("ResponseMetadata").toObject();
    if (json_truthy(meta.value("Error"))) {
        const auto error = meta.value("Error").toObject();
        return TranslationResult::failure(map_api_error_code(first_text({error.value("Code")})), first_text({error.value("Message")}, "Volcengine MT API error"));
    }
    const auto results = raw.value("TranslationList").toArray();
    if (results.isEmpty()) return TranslationResult::failure(TranslationErrorCode::UNKNOWN, INVALID_RESPONSE);
    QStringList parts;
    for (const auto &item : results)
        if (item.isObject()) parts << first_text({item.toObject().value("Translation")});
    const auto translated = parts.join("\n");
    if (translated.isEmpty()) return TranslationResult::failure(TranslationErrorCode::UNKNOWN, INVALID_RESPONSE);
    const auto first = results.first();
    const auto detected = first.isObject() ? from_code(first_text({first.toObject().value("DetectedSourceLanguage")})) : QString();
    return TranslationResult{true, translated, detected, std::nullopt, {}};
}

// Volcengine mostly uses ISO codes, so only Chinese needs mapping.
QString VolcengineTranslateProvider::to_code(const QString &language_code) {
    static const QHash<QString, QString> codes = {{"zh-Hans", "zh"}, {"zh-Hant", "zh-Hant"}};
    return codes.value(language_code, language_code);
}

QString VolcengineTranslateProvider::from_code(const QString &code) {
    static const QHash<QString, QString> codes = {{"zh", "zh-Hans"}, {"zh-Hant", "zh-Hant"}};
    if (code.isEmpty()) return {};
    return codes.value(code, code);
}

TranslationResult VolcengineTranslateProvider::http_error(const HttpResponse &response) const {
    const auto data = error_body_object(response.body);
    if (json_truthy(data.value("ResponseMetadata").toObject().value("Error"))) return parse_response(data);
    const auto message = response.reason.isEmpty() ? QString("HTTP %1").arg(response.status) : response.reason;
    const auto code = response.status == 401 || response.status == 403 ? TranslationErrorCode::AUTH_FAILED
                      : response.status >= 500                          ? TranslationErrorCode::NETWORK_ERROR
                                                                        : TranslationErrorCode::UNKNOWN;
    log_error(QString("Volcengine MT HTTP %1: %2").arg(response.status).arg(message), "VolcengineTranslate");
    return TranslationResult::failure(code, message);
}

TranslationErrorCode VolcengineTranslateProvider::map_api_error_code(const QString &code) {
    // From the text translation API: -400 bad parameter, -415 unsupported pair,
    // -429 too frequent, -500 internal error, 1000XX interface errors.
    if (code == "-415") return TranslationErrorCode::UNSUPPORTED_LANGUAGE;
    if (code == "-429") return TranslationErrorCode::RATE_LIMITED;
    if (code == "-400") return TranslationErrorCode::INVALID_REQUEST;
    if (code == "-500") return TranslationErrorCode::NETWORK_ERROR;
    return TranslationErrorCode::UNKNOWN;
}
}
