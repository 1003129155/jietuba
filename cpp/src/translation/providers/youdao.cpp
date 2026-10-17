#include "translation/providers/youdao.h"
#include "core/logger.h"
#include "translation/providers/common.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QHash>
#include <QSet>
#include <QUuid>

namespace jietuba {
namespace {
const QString INVALID_RESPONSE = "Invalid Youdao Translate response";

// The service answers with a code only; the messages shown to the user are ours.
const QHash<QString, QString> &error_messages() {
    static const QHash<QString, QString> messages = {
        {"101", "Missing required parameter"},
        {"102", "Unsupported language"},
        {"103", "Text is too long (up to 5000 characters per request)"},
        {"108", "Invalid application ID"},
        {"110", "The application is not bound to the text translation service"},
        {"111", "Invalid developer account"},
        {"112", "Invalid service requested"},
        {"113", "Text is empty"},
        {"202", "Signature verification failed, check the application ID and secret"},
        {"203", "Request IP is not in the application's allowed IP list"},
        {"206", "Invalid timestamp, check the system clock"},
        {"302", "Translation query failed"},
        {"303", "Youdao server error"},
        {"304", "Youdao session timed out"},
        {"401", "Youdao account is in arrears, please top up"},
        {"411", "Request rate limited"},
        {"412", "Long requests are too frequent"},
    };
    return messages;
}

bool has_astral(const QString &text) {
    for (qsizetype i = 0; i + 1 < text.size(); ++i)
        if (text.at(i).isHighSurrogate() && text.at(i + 1).isLowSurrogate()) return true;
    return false;
}

QString sign_input(const QString &text, bool utf16) {
    if (utf16) {
        if (text.size() <= 20) return text;
        return text.left(10) + QString::number(text.size()) + text.right(10);
    }
    const auto points = text.toUcs4();
    if (points.size() <= 20) return text;
    const auto *data = reinterpret_cast<const char32_t *>(points.constData());
    return QString::fromUcs4(data, 10) + QString::number(points.size()) + QString::fromUcs4(data + points.size() - 10, 10);
}

// UTF-8 with each unpaired surrogate written as '?': cutting at 10 UTF-16 units can split a pair.
QByteArray utf8_replacing_lone_surrogates(const QString &text) {
    QByteArray out;
    out.reserve(text.size() * 3);
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c.isHighSurrogate() && i + 1 < text.size() && text.at(i + 1).isLowSurrogate()) {
            out += text.mid(i, 2).toUtf8();
            ++i;
        } else if (c.isSurrogate()) {
            out += '?';
        } else {
            out += QString(c).toUtf8();
        }
    }
    return out;
}
}

YoudaoTranslateProvider::YoudaoTranslateProvider(const ProviderConfig &config)
    : app_key_(config.value("app_key").toString().trimmed()), app_secret_(config.value("app_secret").toString().trimmed()) {
    min_timeout = 5;
}

ProviderMetadata YoudaoTranslateProvider::class_metadata() {
    ProviderMetadata metadata;
    metadata.provider_id = "youdao";
    metadata.display_name = "Youdao";
    metadata.credentials = {
        ProviderField::text("youdao_translate_app_key", "Youdao App ID", "ID of an application bound to Text Translation (Application Management)"),
        ProviderField::text("youdao_translate_app_secret", "Youdao App Secret", "Application secret, paired with the application ID", true),
    };
    metadata.notice = "Youdao AI Cloud: new accounts get a trial credit, after that translation is billed by character count.";
    metadata.help_label = "Youdao AI Cloud Console";
    metadata.help_url = "https://ai.youdao.com/console/";
    return metadata;
}

TranslationResult YoudaoTranslateProvider::translate(const TranslationRequest &request) {
    const int timeout = request.timeout;
    return translate_with(request, [timeout](const QByteArray &form) {
        return http_request("POST", API_URL, {{"Content-Type", CONTENT_TYPE}}, form, timeout);
    });
}

TranslationResult YoudaoTranslateProvider::translate_with(const TranslationRequest &request, const Send &send) const {
    if (request.text.trimmed().isEmpty()) return TranslationResult::failure(TranslationErrorCode::INVALID_REQUEST, "Text is empty");
    if (!is_configured()) return TranslationResult::failure(TranslationErrorCode::NOT_CONFIGURED, "Youdao App ID/App Secret is not configured");

    const auto attempt = [&](bool utf16_input) {
        const auto salt = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto curtime = QString::number(QDateTime::currentSecsSinceEpoch());
        return handle_response(send(request_form(request, salt, curtime, utf16_input)), request.timeout);
    };
    auto [result, api_code] = attempt(true);
    // The documentation does not settle whether the signed input counts UTF-16 units or code
    // points. The two differ only for text outside the BMP, so a signature rejected there (202)
    // is retried once the other way.
    if (api_code == "202" && has_astral(request.text)) result = attempt(false).first;
    return result;
}

QByteArray YoudaoTranslateProvider::request_form(const TranslationRequest &request, const QString &salt, const QString &curtime, bool utf16_input) const {
    return python_urlencode({
                                {"q", request.text},
                                {"from", request.source_lang ? to_code(*request.source_lang) : QString("auto")},
                                {"to", to_code(request.target_lang)},
                                {"appKey", app_key_},
                                {"salt", salt},
                                {"sign", sign(request.text, salt, curtime, utf16_input)},
                                {"signType", "v3"},
                                {"curtime", curtime},
                            })
        .toUtf8();
}

QString YoudaoTranslateProvider::sign(const QString &text, const QString &salt, const QString &curtime, bool utf16_input) const {
    const auto raw = app_key_ + sign_input(text, utf16_input) + salt + curtime + app_secret_;
    return QString::fromLatin1(QCryptographicHash::hash(utf8_replacing_lone_surrogates(raw), QCryptographicHash::Sha256).toHex());
}

std::pair<TranslationResult, QString> YoudaoTranslateProvider::handle_response(const HttpResponse &response, int timeout) const {
    if (response.kind == HttpResponse::HttpError) return {http_error(response), {}};
    if (response.kind == HttpResponse::Timeout) {
        log_error(QString("Youdao Translate timed out after %1s").arg(timeout), "YoudaoTranslate");
        return {TranslationResult::failure(TranslationErrorCode::NETWORK_ERROR, QString("Request timed out after %1s").arg(timeout)), {}};
    }
    if (response.kind == HttpResponse::NetworkError) {
        log_error("Youdao Translate network error: " + response.reason, "YoudaoTranslate");
        return {TranslationResult::failure(TranslationErrorCode::NETWORK_ERROR, "Network error: " + response.reason), {}};
    }
    const auto raw = parse_json_body(response.body);
    if (!raw) {
        log_error(QString("Youdao Translate response error: invalid JSON"), "YoudaoTranslate");
        return {TranslationResult::failure(TranslationErrorCode::UNKNOWN, "Failed to parse Youdao Translate response"), {}};
    }
    const QString api_code = raw->isObject() ? json_text(raw->toObject().value("errorCode")) : QString();
    return {parse_response(*raw), api_code};
}

TranslationResult YoudaoTranslateProvider::parse_response(const QJsonValue &raw_value) const {
    // Authentication and parameter errors also come back as HTTP 200, with errorCode in the
    // body ("0" for success).
    if (!raw_value.isObject()) return TranslationResult::failure(TranslationErrorCode::UNKNOWN, INVALID_RESPONSE);
    const auto raw = raw_value.toObject();
    const auto error_code = json_text(raw.value("errorCode"));
    if (error_code != "0") {
        const auto message = error_messages().value(error_code, "Youdao error " + (error_code.isEmpty() ? QString("unknown") : error_code));
        log_error(QString("Youdao Translate API error %1: %2").arg(error_code, message), "YoudaoTranslate");
        return TranslationResult::failure(map_api_error_code(error_code), message);
    }
    QStringList parts;
    for (const auto &item : raw.value("translation").toArray()) parts << json_text(item);
    const auto translated = parts.join("\n");
    if (translated.trimmed().isEmpty()) return TranslationResult::failure(TranslationErrorCode::UNKNOWN, INVALID_RESPONSE);
    // l looks like "EN2zh-CHS" or "auto2zh-CHS"; no language code contains the digit 2.
    const auto source = json_text(raw.value("l")).section('2', 0, 0);
    return TranslationResult{true, translated, from_code(source), std::nullopt, {}};
}

// Only codes that differ from the application's are listed; others pass through. English and
// Portuguese variants collapse into one Youdao code, so the reverse map is separate.
QString YoudaoTranslateProvider::to_code(const QString &language_code) {
    static const QHash<QString, QString> codes = {{"zh-Hans", "zh-CHS"}, {"zh-Hant", "zh-CHT"}, {"en-US", "en"}, {"en-GB", "en"}, {"pt-BR", "pt"}, {"pt-PT", "pt"}};
    return codes.value(language_code, language_code);
}

// The response's case is not fixed, so codes are compared in lower case.
QString YoudaoTranslateProvider::from_code(const QString &code) {
    static const QHash<QString, QString> codes = {{"zh-chs", "zh-Hans"}, {"zh-cht", "zh-Hant"}};
    const auto lower = code.toLower();
    if (lower.isEmpty() || lower == "auto") return {};
    return codes.value(lower, lower);
}

TranslationResult YoudaoTranslateProvider::http_error(const HttpResponse &response) const {
    const auto message = response.reason.isEmpty() ? QString("HTTP %1").arg(response.status) : response.reason;
    const auto code = response.status == 401 || response.status == 403 ? TranslationErrorCode::AUTH_FAILED
                      : response.status >= 500                          ? TranslationErrorCode::NETWORK_ERROR
                                                                        : TranslationErrorCode::UNKNOWN;
    log_error(QString("Youdao Translate HTTP %1: %2").arg(response.status).arg(message), "YoudaoTranslate");
    return TranslationResult::failure(code, message);
}

// From the error code list in Youdao AI Cloud's text translation documentation.
TranslationErrorCode YoudaoTranslateProvider::map_api_error_code(const QString &code) {
    static const QSet<QString> auth = {"108", "110", "111", "112", "202", "203"}, rate = {"411", "412"}, invalid = {"101", "103", "113", "206"},
                               network = {"302", "303", "304"};
    if (auth.contains(code)) return TranslationErrorCode::AUTH_FAILED;
    if (code == "401") return TranslationErrorCode::QUOTA_EXCEEDED;
    if (rate.contains(code)) return TranslationErrorCode::RATE_LIMITED;
    if (code == "102") return TranslationErrorCode::UNSUPPORTED_LANGUAGE;
    if (invalid.contains(code)) return TranslationErrorCode::INVALID_REQUEST;
    if (network.contains(code)) return TranslationErrorCode::NETWORK_ERROR;
    return TranslationErrorCode::UNKNOWN;
}
}
