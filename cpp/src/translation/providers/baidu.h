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
    // The service translates every line of q on its own and answers with one entry per
    // line, so a batch travels as one q and its texts are told apart by their line counts.
    BatchLimits batch_limits() const override { return {100, 2000}; }
    BatchTranslationResult translate_batch(const QStringList &texts, const TranslationRequest &request) override;
    // The lines of all texts in order, with how many each text has; nullopt when a text
    // has a blank line, which the service would drop from its reply.
    static std::optional<QStringList> batch_lines(const QStringList &texts, QVector<int> *line_counts);
    // The reply's entries back into one translation per text.
    static std::optional<QStringList> batch_translations(const QStringList &entries, const QVector<int> &line_counts);
    // entries receives the reply's translations, one per line of q.
    TranslationResult parse_response(const QJsonValue &raw, QStringList *entries = nullptr) const;
    // MD5(appid + q + salt + key), lowercase hex. The signature uses salt's decimal
    // text while the JSON body must carry the number itself.
    QString sign(const QString &text, int salt) const;
private:
    // One exchange for q.
    TranslationResult exchange(const QString &q, const TranslationRequest &request, QStringList *entries) const;
    TranslationResult http_error(const HttpResponse &response) const;
    static QString to_baidu_code(const QString &language_code);
    static QString from_baidu_code(const QString &baidu_code);
    static TranslationErrorCode map_api_error_code(const QString &error_code);
    QString appid_;
    QString secret_key_;
};
}
