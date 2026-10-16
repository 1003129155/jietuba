#include "translation/providers/amazon.h"
#include "core/http_client.h"
#include "core/logger.h"
#include "translation/providers/common.h"
#include <QCryptographicHash>
#include <QHash>
#include <QMessageAuthenticationCode>
#include <map>

namespace jietuba {
namespace {
QByteArray hmac_sha256(const QByteArray &key, const QByteArray &message) {
    return QMessageAuthenticationCode::hash(message, key, QCryptographicHash::Sha256);
}
QString sha256_hex(const QByteArray &data) { return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex()); }
}

AmazonTranslateProvider::AmazonTranslateProvider(const ProviderConfig &config)
    : region_(config.value("region", "us-west-2").toString().trimmed()), access_key_id_(config.value("access_key_id").toString().trimmed()),
      secret_access_key_(config.value("secret_access_key").toString().trimmed()), session_token_(config.value("session_token").toString().trimmed()) {}

ProviderMetadata AmazonTranslateProvider::class_metadata() {
    ProviderMetadata metadata;
    metadata.provider_id = "amazon";
    metadata.display_name = "Amazon";
    metadata.credentials = {
        ProviderField::text("amazon_translate_region", QString::fromUtf8("AWS 区域"), "us-west-2"),
        ProviderField::text("amazon_translate_access_key_id", "Access Key ID", "AKIA..."),
        ProviderField::text("amazon_translate_secret_access_key", "Secret Access Key", "Secret Access Key", true),
        ProviderField::text("amazon_translate_session_token", "Session Token", QString::fromUtf8("可选，临时凭据使用"), true),
    };
    metadata.help_label = "console.aws.amazon.com";
    metadata.help_url = "https://console.aws.amazon.com/iam/home#/security_credentials";
    return metadata;
}

TranslationResult AmazonTranslateProvider::translate(const TranslationRequest &request) {
    if (request.text.trimmed().isEmpty()) return TranslationResult::failure(TranslationErrorCode::INVALID_REQUEST, "Text is empty");
    if (request.text.toUtf8().size() > MAX_TEXT_BYTES)
        return TranslationResult::failure(TranslationErrorCode::INVALID_REQUEST, "Text exceeds Amazon Translate's 10,000-byte limit");
    if (!is_configured()) return TranslationResult::failure(TranslationErrorCode::NOT_CONFIGURED, "Amazon Translate credentials are not configured");

    const auto payload = compact_json(QJsonObject{
        {"Text", request.text},
        {"SourceLanguageCode", to_amazon_code(request.source_lang)},
        {"TargetLanguageCode", to_amazon_code(request.target_lang)},
    });
    const auto [url, signed_headers] = signed_request(payload);
    // The HTTP stack always sends Host for the URL, which is the signed value.
    HttpHeaders headers;
    for (const auto &header : signed_headers)
        if (header.first != "host") headers.push_back(header);
    const auto response = http_request("POST", url, headers, payload, request.timeout);
    if (response.kind == HttpResponse::HttpError) return http_error(response);
    if (response.kind == HttpResponse::Timeout) {
        log_error(QString("Amazon Translate timed out after %1s").arg(request.timeout), "AmazonTranslate");
        return TranslationResult::failure(TranslationErrorCode::NETWORK_ERROR, QString("Request timed out after %1s").arg(request.timeout));
    }
    if (response.kind == HttpResponse::NetworkError) {
        log_error("Amazon Translate network error: " + response.reason, "AmazonTranslate");
        return TranslationResult::failure(TranslationErrorCode::NETWORK_ERROR, "Network error: " + response.reason);
    }
    const auto raw = parse_json_body(response.body);
    if (!raw) {
        log_error(QString("Amazon Translate response error: invalid JSON"), "AmazonTranslate");
        return TranslationResult::failure(TranslationErrorCode::UNKNOWN, "Failed to parse Amazon Translate response");
    }
    const auto object = raw->toObject();
    const auto translated = json_text(object.value("TranslatedText"));
    if (translated.isEmpty()) return TranslationResult::failure(TranslationErrorCode::UNKNOWN, "Invalid Amazon Translate response");
    const auto detected = normalize_language_code(object.value("SourceLanguageCode").toString());
    return TranslationResult{true, translated, detected.value_or(QString()), std::nullopt, {}};
}

std::pair<QString, std::vector<std::pair<QString, QString>>> AmazonTranslateProvider::signed_request(const QByteArray &payload, const QDateTime &now_in) const {
    const auto now = (now_in.isValid() ? now_in : QDateTime::currentDateTimeUtc()).toUTC();
    const auto amz_date = now.toString("yyyyMMdd'T'HHmmss'Z'");
    const auto date_stamp = now.toString("yyyyMMdd");

    const auto host_name = host();
    std::map<QString, QString> headers = {
        {"content-type", CONTENT_TYPE},
        {"host", host_name},
        {"x-amz-date", amz_date},
        {"x-amz-target", TARGET},
    };
    if (!session_token_.isEmpty()) headers["x-amz-security-token"] = session_token_;

    QString canonical_header_text;
    QStringList names;
    for (const auto &[name, value] : headers) {
        canonical_header_text += name + ":" + value.simplified() + "\n";
        names << name;
    }
    const auto signed_headers = names.join(";");
    const auto canonical_request = QStringList{"POST", "/", "", canonical_header_text, signed_headers, sha256_hex(payload)}.join("\n");
    const auto credential_scope = date_stamp + "/" + region_ + "/" + SERVICE + "/aws4_request";
    const auto string_to_sign = QStringList{ALGORITHM, amz_date, credential_scope, sha256_hex(canonical_request.toUtf8())}.join("\n");
    const auto signature = QString::fromLatin1(hmac_sha256(signing_key(date_stamp), string_to_sign.toUtf8()).toHex());

    std::vector<std::pair<QString, QString>> result(headers.begin(), headers.end());
    result.emplace_back("authorization", ALGORITHM + " Credential=" + access_key_id_ + "/" + credential_scope + ", SignedHeaders=" + signed_headers +
                                             ", Signature=" + signature);
    return {"https://" + host_name + "/", result};
}

QByteArray AmazonTranslateProvider::signing_key(const QString &date_stamp) const {
    const auto key_date = hmac_sha256(("AWS4" + secret_access_key_).toUtf8(), date_stamp.toUtf8());
    const auto key_region = hmac_sha256(key_date, region_.toUtf8());
    const auto key_service = hmac_sha256(key_region, SERVICE.toUtf8());
    return hmac_sha256(key_service, "aws4_request");
}

QString AmazonTranslateProvider::host() const {
    const QString suffix = region_.startsWith("cn-") ? "amazonaws.com.cn" : "amazonaws.com";
    return "translate." + region_ + "." + suffix;
}

QString AmazonTranslateProvider::to_amazon_code(const std::optional<QString> &language_code) {
    static const QHash<QString, QString> codes = {{"zh-Hans", "zh"}, {"zh-Hant", "zh-TW"}, {"en-US", "en"}, {"en-GB", "en"}, {"pt-BR", "pt"}};
    if (!language_code || language_code->isEmpty()) return "auto";
    return codes.value(*language_code, *language_code);
}

TranslationResult AmazonTranslateProvider::http_error(const HttpResponse &response) const {
    const auto data = error_body_object(response.body);
    const auto error_type = first_text({data.value("__type"), data.value("code"), data.value("Code")}).split('#').last();
    const auto message = first_text({data.value("message"), data.value("Message"), QJsonValue(response.reason)}, QString("HTTP %1").arg(response.status));
    const auto code = map_error_code(error_type, response.status);
    log_error(QString("Amazon Translate HTTP %1: %2").arg(response.status).arg(error_type.isEmpty() ? message : error_type), "AmazonTranslate");
    return TranslationResult::failure(code, message);
}

TranslationErrorCode AmazonTranslateProvider::map_error_code(const QString &error_type, int status_code) {
    static const QSet<QString> auth = {"AccessDeniedException", "InvalidSignatureException", "UnrecognizedClientException", "IncompleteSignature"};
    static const QSet<QString> rate = {"TooManyRequestsException", "ThrottlingException"};
    static const QSet<QString> invalid = {"InvalidRequestException", "TextSizeLimitExceededException", "DetectedLanguageLowConfidenceException"};
    static const QSet<QString> network = {"InternalServerException", "ServiceUnavailableException"};
    if (auth.contains(error_type) || status_code == 401 || status_code == 403) return TranslationErrorCode::AUTH_FAILED;
    if (rate.contains(error_type) || status_code == 429) return TranslationErrorCode::RATE_LIMITED;
    if (error_type == "UnsupportedLanguagePairException") return TranslationErrorCode::UNSUPPORTED_LANGUAGE;
    if (invalid.contains(error_type) || status_code == 400) return TranslationErrorCode::INVALID_REQUEST;
    if (network.contains(error_type) || status_code >= 500) return TranslationErrorCode::NETWORK_ERROR;
    return TranslationErrorCode::UNKNOWN;
}
}
