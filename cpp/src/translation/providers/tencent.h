#pragma once
#include "core/http_client.h"
#include "translation/provider.h"

class QJsonValue;

namespace jietuba {
// Tencent Cloud machine translation (TMT TextTranslate) signed with API 3.0 TC3-HMAC-SHA256.
class TencentTranslateProvider : public TranslationProvider {
public:
    static inline const QString API_URL = "https://tmt.tencentcloudapi.com";
    static inline const QString HOST = "tmt.tencentcloudapi.com";
    static inline const QString SERVICE = "tmt";
    static inline const QString ACTION = "TextTranslate";
    static inline const QString VERSION = "2018-03-21";
    static inline const QString REGION = "ap-guangzhou";
    static constexpr int PROJECT_ID = 0;
    // About 5M free characters a month, less a 10k margin.
    static constexpr qint64 QUOTA_LIMIT_CHARS = 4990000;
    explicit TencentTranslateProvider(const ProviderConfig &config);
    static ProviderMetadata class_metadata();
    QString provider_id() const override { return "tencent"; }
    QString display_name() const override { return "Tencent"; }
    bool is_configured() const override { return !secret_id_.isEmpty() && !secret_key_.isEmpty(); }
    TranslationResult translate(const TranslationRequest &request) override;
    static QString request_url() { return API_URL + "/"; }
    QByteArray request_payload(const TranslationRequest &request) const;
    // The request headers, authorization included, for payload signed at timestamp
    // (Unix seconds), in the order they are sent.
    HttpHeaders signed_headers(const QByteArray &payload, qint64 timestamp) const;
    // The result of one exchange; timeout only names the limit in the timeout message.
    TranslationResult handle_response(const HttpResponse &response, int timeout) const;
    TranslationResult parse_response(const QJsonValue &raw) const;
    static QString to_code(const QString &language_code);
    static QString from_code(const QString &code);
    static TranslationErrorCode map_api_error_code(const QString &code);
private:
    TranslationResult http_error(const HttpResponse &response) const;
    QString secret_id_;
    QString secret_key_;
};
}
