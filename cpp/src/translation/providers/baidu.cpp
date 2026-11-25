#include "translation/providers/baidu.h"
#include "core/http_client.h"
#include "core/logger.h"
#include "translation/providers/common.h"
#include <QCryptographicHash>
#include <QHash>
#include <QRandomGenerator>

namespace jietuba {
namespace {
// A batch carries up to a hundred texts, so a capture takes few requests. An account without
// the advanced tier is limited to one a second and is slowed down by the rate limit reply.
constexpr RequestPacing PACING{4, 50};
}

BaiduTranslateProvider::BaiduTranslateProvider(const ProviderConfig &config)
    : appid_(config.value("appid").toString().trimmed()), secret_key_(config.value("secret_key").toString().trimmed()) {
    apply_pacing(PACING, config);
}

ProviderMetadata BaiduTranslateProvider::class_metadata() {
    ProviderMetadata metadata;
    metadata.provider_id = "baidu";
    metadata.display_name = "Baidu";
    metadata.credentials = {
        ProviderField::text("baidu_translate_appid", "Baidu APPID", "APPID"),
        // The console calls this the secret key; Baidu's separate "API Key" is a
        // different credential that fails with the same generic 54001.
        ProviderField::text("baidu_translate_secret_key", "Baidu Secret Key", "Paired with APPID in the Baidu console", true),
    };
    metadata.pacing = pacing_fields("baidu_translate", PACING);
    metadata.help_label = "Baidu Translate Open Platform · Developer Info";
    metadata.help_url = "https://fanyi-api.baidu.com/manage/developer";
    return metadata;
}

TranslationResult BaiduTranslateProvider::translate(const TranslationRequest &request) {
    if (request.text.trimmed().isEmpty()) return TranslationResult::failure(TranslationErrorCode::INVALID_REQUEST, "Text is empty");
    if (!is_configured()) return TranslationResult::failure(TranslationErrorCode::NOT_CONFIGURED, "Baidu Translate APPID/Secret Key is not configured");
    return exchange(request.text, request, nullptr);
}

BatchTranslationResult BaiduTranslateProvider::translate_batch(const QStringList &texts, const TranslationRequest &request) {
    if (texts.isEmpty()) return BatchTranslationResult::failure(TranslationErrorCode::INVALID_REQUEST, "Text is empty");
    if (!is_configured()) return BatchTranslationResult::failure(TranslationErrorCode::NOT_CONFIGURED, "Baidu Translate APPID/Secret Key is not configured");
    QVector<int> line_counts;
    const auto lines = batch_lines(texts, &line_counts);
    if (!lines) return BatchTranslationResult::failure(TranslationErrorCode::INVALID_REQUEST, "Text has an empty line");
    QStringList entries;
    const auto reply = exchange(lines->join('\n'), request, &entries);
    if (!reply.success) return BatchTranslationResult::failure(reply);
    const auto translations = batch_translations(entries, line_counts);
    if (!translations) return BatchTranslationResult::failure(TranslationErrorCode::UNKNOWN, "Invalid Baidu Translate response");
    return BatchTranslationResult{true, *translations, reply.detected_source_lang, std::nullopt, {}};
}

std::optional<QStringList> BaiduTranslateProvider::batch_lines(const QStringList &texts, QVector<int> *line_counts) {
    QStringList lines;
    for (const auto &text : texts) {
        const auto own = text.split('\n');
        for (const auto &line : own)
            if (line.trimmed().isEmpty()) return std::nullopt;
        lines += own;
        line_counts->append(static_cast<int>(own.size()));
    }
    return lines;
}

std::optional<QStringList> BaiduTranslateProvider::batch_translations(const QStringList &entries, const QVector<int> &line_counts) {
    int total = 0;
    for (const int count : line_counts) total += count;
    // One entry per line sent: anything else cannot be paired up.
    if (entries.size() != total) return std::nullopt;
    QStringList translations;
    int next = 0;
    for (const int count : line_counts) {
        translations << entries.mid(next, count).join('\n');
        next += count;
    }
    return translations;
}

TranslationResult BaiduTranslateProvider::exchange(const QString &q, const TranslationRequest &request, QStringList *entries) const {
    // salt must be a JSON number and appid a JSON string; either the other way
    // round fails with 53001.
    const int salt = QRandomGenerator::global()->bounded(100000, 1000000);
    const QJsonObject body{
        {"appid", appid_},
        {"from", request.source_lang ? to_baidu_code(*request.source_lang) : QString("auto")},
        {"to", to_baidu_code(request.target_lang)},
        {"q", q},
        {"salt", salt},
        {"sign", sign(q, salt)},
    };
    const auto response = http_request("POST", API_URL, {{"Content-Type", "application/json"}}, compact_json(body), request.timeout);
    if (response.kind == HttpResponse::HttpError) return http_error(response);
    if (response.kind == HttpResponse::Timeout) {
        log_error(QString("Baidu Translate timed out after %1s").arg(request.timeout), "BaiduTranslate");
        return TranslationResult::failure(TranslationErrorCode::NETWORK_ERROR, QString("Request timed out after %1s").arg(request.timeout));
    }
    if (response.kind == HttpResponse::NetworkError) {
        log_error("Baidu Translate network error: " + response.reason, "BaiduTranslate");
        return TranslationResult::failure(TranslationErrorCode::NETWORK_ERROR, "Network error: " + response.reason);
    }
    const auto raw = parse_json_body(response.body);
    if (!raw) {
        log_error(QString("Baidu Translate response error: invalid JSON"), "BaiduTranslate");
        return TranslationResult::failure(TranslationErrorCode::UNKNOWN, "Failed to parse Baidu Translate response");
    }
    return parse_response(*raw, entries);
}

QString BaiduTranslateProvider::sign(const QString &text, int salt) const {
    const auto raw = appid_ + text + QString::number(salt) + secret_key_;
    return QString::fromLatin1(QCryptographicHash::hash(raw.toUtf8(), QCryptographicHash::Md5).toHex());
}

TranslationResult BaiduTranslateProvider::parse_response(const QJsonValue &raw_value, QStringList *entries) const {
    // Authentication and parameter errors also come back as HTTP 200 with
    // error_code/error_msg in the body.
    if (!raw_value.isObject()) return TranslationResult::failure(TranslationErrorCode::UNKNOWN, "Invalid Baidu Translate response");
    const auto raw = raw_value.toObject();
    const auto error_code = raw.value("error_code");
    if (json_truthy(error_code) && json_text(error_code) != "0") {
        const auto code_text = json_text(error_code);
        const auto message = first_text({raw.value("error_msg")}, "error_code=" + code_text);
        log_error(QString("Baidu Translate API error %1: %2").arg(code_text, message), "BaiduTranslate");
        return TranslationResult::failure(map_api_error_code(code_text), message);
    }
    const auto results = raw.value("trans_result").toArray();
    if (results.isEmpty()) return TranslationResult::failure(TranslationErrorCode::UNKNOWN, "Invalid Baidu Translate response");
    QStringList parts;
    for (const auto &item : results) parts << json_text(item.toObject().value("dst"));
    const auto translated = parts.join("\n");
    if (translated.isEmpty()) return TranslationResult::failure(TranslationErrorCode::UNKNOWN, "Invalid Baidu Translate response");
    if (entries) *entries = parts;
    const auto detected = from_baidu_code(json_text(raw.value("from")));
    return TranslationResult{true, translated, detected, std::nullopt, {}};
}

// Only codes that differ from the application's are listed; others pass through.
// Each direction is spelled out because several application codes collapse into
// one Baidu code. Pass-through was checked against the live API for every target
// language: only Ukrainian needs mapping, tr/id/th/nl pass through as is.
QString BaiduTranslateProvider::to_baidu_code(const QString &language_code) {
    static const QHash<QString, QString> codes = {
        {"zh-Hans", "zh"}, {"zh-Hant", "cht"}, {"ja", "jp"}, {"ko", "kor"}, {"vi", "vie"}, {"fr", "fra"}, {"es", "spa"},
        {"ar", "ara"}, {"uk", "ukr"}, {"en-US", "en"}, {"en-GB", "en"}, {"pt-BR", "pt"}, {"pt-PT", "pt"},
    };
    return codes.value(language_code, language_code);
}

QString BaiduTranslateProvider::from_baidu_code(const QString &baidu_code) {
    static const QHash<QString, QString> codes = {
        {"zh", "zh-Hans"}, {"cht", "zh-Hant"}, {"jp", "ja"}, {"kor", "ko"}, {"vie", "vi"}, {"fra", "fr"}, {"spa", "es"}, {"ara", "ar"}, {"ukr", "uk"},
    };
    if (baidu_code.isEmpty()) return {};
    return codes.value(baidu_code, baidu_code);
}

TranslationResult BaiduTranslateProvider::http_error(const HttpResponse &response) const {
    const auto parsed = response.body.isEmpty() ? std::nullopt : parse_json_body(response.body);
    if (parsed && parsed->isObject() && json_truthy(parsed->toObject().value("error_code"))) return parse_response(*parsed);
    const auto message = response.reason.isEmpty() ? QString("HTTP %1").arg(response.status) : response.reason;
    const auto code = response.status == 401 || response.status == 403 ? TranslationErrorCode::AUTH_FAILED
                      : response.status >= 500                          ? TranslationErrorCode::NETWORK_ERROR
                                                                        : TranslationErrorCode::UNKNOWN;
    log_error(QString("Baidu Translate HTTP %1: %2").arg(response.status).arg(message), "BaiduTranslate");
    return TranslationResult::failure(code, message);
}

TranslationErrorCode BaiduTranslateProvider::map_api_error_code(const QString &error_code) {
    // Codes from https://fanyi-api.baidu.com/doc/21; 53001 (unparseable body) and 59004
    // (over the account's requests per second) are undocumented but returned by the live API.
    static const QSet<QString> auth = {"52003", "54001", "58000"}, invalid = {"53001", "54000"}, rate = {"54003", "54005", "59004"},
                               network = {"52001", "52002"};
    if (auth.contains(error_code)) return TranslationErrorCode::AUTH_FAILED;
    if (invalid.contains(error_code)) return TranslationErrorCode::INVALID_REQUEST;
    if (rate.contains(error_code)) return TranslationErrorCode::RATE_LIMITED;
    if (error_code == "54004") return TranslationErrorCode::QUOTA_EXCEEDED;
    if (error_code == "58001") return TranslationErrorCode::UNSUPPORTED_LANGUAGE;
    if (network.contains(error_code)) return TranslationErrorCode::NETWORK_ERROR;
    return TranslationErrorCode::UNKNOWN;
}
}
