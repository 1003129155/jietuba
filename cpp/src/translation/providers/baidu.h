#pragma once
#include "translation/provider.h"

class QJsonValue;

namespace jietuba {
struct HttpResponse;

// Baidu large-model text translation (aiTextTranslate), authenticated with the
// APPID + secret key MD5 signature. The endpoint also takes a Bearer token, but that
// needs a separately issued key; with the standard credentials it fails with a
// generic 54001, so only the signature path is offered.
class BaiduTranslateProvider : public TranslationProvider {
public:
    static inline const QString API_URL = "https://fanyi-api.baidu.com/ait/api/aiTextTranslate";
    explicit BaiduTranslateProvider(const ProviderConfig &config);
    static ProviderMetadata class_metadata();
    QString provider_id() const override { return "baidu"; }
    QString display_name() const override { return "Baidu"; }
    bool is_configured() const override { return !appid_.isEmpty() && !secret_key_.isEmpty(); }
    TranslationResult translate(const TranslationRequest &request) override;
    // MD5(appid + q + salt + key), lowercase hex. The signature uses salt's decimal
    // text while the JSON body must carry the number itself.
    QString sign(const QString &text, int salt) const;
private:
    TranslationResult parse_response(const QJsonValue &raw) const;
    TranslationResult http_error(const HttpResponse &response) const;
    static QString to_baidu_code(const QString &language_code);
    static QString from_baidu_code(const QString &baidu_code);
    static TranslationErrorCode map_api_error_code(const QString &error_code);
    QString appid_;
    QString secret_key_;
};
}
