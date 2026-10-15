#pragma once
#include "translation/provider.h"
#include <QDateTime>
#include <utility>

namespace jietuba {
struct HttpResponse;

// Amazon Translate signed with AWS Signature Version 4.
class AmazonTranslateProvider : public TranslationProvider {
public:
    static inline const QString SERVICE = "translate";
    static inline const QString TARGET = "AWSShineFrontendService_20170701.TranslateText";
    static inline const QString CONTENT_TYPE = "application/x-amz-json-1.1";
    static inline const QString ALGORITHM = "AWS4-HMAC-SHA256";
    static constexpr int MAX_TEXT_BYTES = 10000;
    explicit AmazonTranslateProvider(const ProviderConfig &config);
    static ProviderMetadata class_metadata();
    QString provider_id() const override { return "amazon"; }
    QString display_name() const override { return "Amazon"; }
    bool is_configured() const override { return !region_.isEmpty() && !access_key_id_.isEmpty() && !secret_access_key_.isEmpty(); }
    TranslationResult translate(const TranslationRequest &request) override;
    // The URL and headers (including authorization) for payload, signed at now (UTC).
    std::pair<QString, std::vector<std::pair<QString, QString>>> signed_request(const QByteArray &payload, const QDateTime &now = QDateTime()) const;
private:
    QByteArray signing_key(const QString &date_stamp) const;
    QString host() const;
    static QString to_amazon_code(const std::optional<QString> &language_code);
    TranslationResult http_error(const HttpResponse &response) const;
    static TranslationErrorCode map_error_code(const QString &error_type, int status_code);
    QString region_;
    QString access_key_id_;
    QString secret_access_key_;
    QString session_token_;
};
}
