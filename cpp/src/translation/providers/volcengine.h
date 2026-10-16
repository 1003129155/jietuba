#pragma once
#include "core/http_client.h"
#include "translation/provider.h"

class QJsonValue;

namespace jietuba {
// Volcengine machine translation (TranslateText) signed with OpenAPI V4 (HMAC-SHA256).
// The credentials are the account's AccessKey ID and Secret Access Key from access
// control, not a large-model API key.
class VolcengineTranslateProvider : public TranslationProvider {
public:
    static inline const QString API_URL = "https://translate.volcengineapi.com";
    static inline const QString HOST = "translate.volcengineapi.com";
    static inline const QString REGION = "cn-north-1";
    static inline const QString SERVICE = "translate";
    static inline const QString ACTION = "TranslateText";
    static inline const QString VERSION = "2020-06-01";
    // About 2M free characters a month, less a 10k margin.
    static constexpr qint64 QUOTA_LIMIT_CHARS = 1990000;
    explicit VolcengineTranslateProvider(const ProviderConfig &config);
    static ProviderMetadata class_metadata();
    QString provider_id() const override { return "volcengine"; }
    QString display_name() const override { return "Volcengine"; }
    bool is_configured() const override { return !access_key_id_.isEmpty() && !secret_access_key_.isEmpty(); }
    TranslationResult translate(const TranslationRequest &request) override;
    static QString request_url() { return API_URL + "/?Action=" + ACTION + "&Version=" + VERSION; }
    QByteArray request_payload(const TranslationRequest &request) const;
    // The request headers, authorization included, for payload signed at x_date
    // (yyyyMMddTHHmmssZ, UTC), in the order they are sent.
    HttpHeaders signed_headers(const QByteArray &payload, const QString &x_date) const;
    // The result of one exchange; timeout only names the limit in the timeout message.
    TranslationResult handle_response(const HttpResponse &response, int timeout) const;
    TranslationResult parse_response(const QJsonValue &raw) const;
    static QString to_code(const QString &language_code);
    static QString from_code(const QString &code);
    static TranslationErrorCode map_api_error_code(const QString &code);
private:
    TranslationResult http_error(const HttpResponse &response) const;
    QString access_key_id_;
    QString secret_access_key_;
};
}
