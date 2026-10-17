#pragma once
#include "core/http_client.h"
#include "translation/provider.h"
#include <functional>
#include <utility>

class QJsonValue;

namespace jietuba {
// Youdao AI Cloud text translation, signed with signType v3 (SHA-256). New accounts get a
// trial credit and are then billed by characters; there is no monthly free quota to guard.
class YoudaoTranslateProvider : public TranslationProvider {
public:
    static inline const QString API_URL = "https://openapi.youdao.com/api";
    static inline const QString CONTENT_TYPE = "application/x-www-form-urlencoded; charset=UTF-8";
    using Send = std::function<HttpResponse(const QByteArray &form)>;
    explicit YoudaoTranslateProvider(const ProviderConfig &config);
    static ProviderMetadata class_metadata();
    QString provider_id() const override { return "youdao"; }
    QString display_name() const override { return "Youdao"; }
    bool is_configured() const override { return !app_key_.isEmpty() && !app_secret_.isEmpty(); }
    TranslationResult translate(const TranslationRequest &request) override;
    // translate() with every exchange going through send, one form per request.
    TranslationResult translate_with(const TranslationRequest &request, const Send &send) const;
    // The form body signed with salt and curtime (Unix seconds as text).
    QByteArray request_form(const TranslationRequest &request, const QString &salt, const QString &curtime, bool utf16_input) const;
    // Lowercase hex SHA-256 of app key + input + salt + curtime + app secret. input is the text up
    // to 20 characters, else the first 10 + the length + the last 10, counted in UTF-16 units
    // when utf16_input, otherwise in code points.
    QString sign(const QString &text, const QString &salt, const QString &curtime, bool utf16_input = true) const;
    // The result of one exchange and the API's errorCode, empty when the API did not answer.
    std::pair<TranslationResult, QString> handle_response(const HttpResponse &response, int timeout) const;
    TranslationResult parse_response(const QJsonValue &raw) const;
    static QString to_code(const QString &language_code);
    static QString from_code(const QString &code);
    static TranslationErrorCode map_api_error_code(const QString &code);
private:
    TranslationResult http_error(const HttpResponse &response) const;
    QString app_key_;
    QString app_secret_;
};
}
