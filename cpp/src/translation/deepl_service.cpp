#include "translation/deepl_service.h"
#include "core/http_client.h"
#include "core/logger.h"
#include "translation/providers/common.h"
#include <QHash>

namespace jietuba {
DeepLResult DeepLService::translate(const QString &text, const QString &target_lang, const std::optional<QString> &source_lang, const QString &split_sentences,
                                    bool preserve_formatting, int timeout) {
    const auto batch = translate_batch({text}, target_lang, source_lang, split_sentences, preserve_formatting, timeout);
    if (!batch.success) return DeepLResult{false, {}, {}, batch.error};
    return DeepLResult{true, batch.translated_texts.first(), batch.detected_source_lang, {}};
}

DeepLBatchResult DeepLService::translate_batch(const QStringList &texts, const QString &target_lang, const std::optional<QString> &source_lang,
                                               const QString &split_sentences, bool preserve_formatting, int timeout) {
    using Failure = DeepLBatchResult;
    qsizetype char_count = 0;
    for (const auto &text : texts) {
        if (text.trimmed().isEmpty()) return Failure{false, {}, {}, "Text is empty"};
        char_count += text.toUcs4().size();
    }
    if (texts.isEmpty()) return Failure{false, {}, {}, "Text is empty"};
    if (api_key.isEmpty()) return Failure{false, {}, {}, "API key not configured"};
    // The API takes the text parameter once per text.
    std::vector<std::pair<QString, QString>> data;
    for (const auto &text : texts) data.emplace_back("text", text);
    data.emplace_back("target_lang", target_lang.toUpper());
    data.emplace_back("split_sentences", split_sentences);
    data.emplace_back("preserve_formatting", preserve_formatting ? "1" : "0");
    if (source_lang && !source_lang->isEmpty()) data.emplace_back("source_lang", source_lang->toUpper());
    const auto post_data = python_urlencode(data).toUtf8();
    log_debug(T("发送翻译请求: {char_count} 字符 -> {target_lang}", {{"char_count", static_cast<int>(char_count)}, {"target_lang", target_lang}}), "DeepL");
    const auto response = http_request("POST", api_url, {{"Content-Type", "application/x-www-form-urlencoded"}, {"Authorization", "DeepL-Auth-Key " + api_key}},
                                       post_data, timeout);
    if (response.kind == HttpResponse::HttpError) {
        const auto error_msg = parse_http_error(response.status, response.reason);
        log_error(T("HTTP 错误: {error_msg}", {{"error_msg", error_msg}}), "DeepL");
        return Failure{false, {}, {}, error_msg};
    }
    if (response.kind == HttpResponse::Timeout) {
        log_error(T("请求超时（{seconds} 秒）", {{"seconds", timeout}}), "DeepL");
        return Failure{false, {}, {}, QString("Request timed out after %1s").arg(timeout)};
    }
    if (response.kind == HttpResponse::NetworkError) {
        log_error(T("网络错误: {reason}", {{"reason", response.reason}}), "DeepL");
        return Failure{false, {}, {}, "Network error: " + response.reason};
    }
    const auto parsed = parse_json_body(response.body);
    if (!parsed) {
        log_error(T("JSON 解析失败: {e}", {{"e", QString("invalid JSON")}}), "DeepL");
        return Failure{false, {}, {}, "Failed to parse API response"};
    }
    const auto translations = parsed->toObject().value("translations").toArray();
    // One translation per text, in the order sent: anything else cannot be paired up.
    if (translations.size() != texts.size()) return Failure{false, {}, {}, "Invalid API response format"};
    QStringList translated_texts;
    for (const auto &translation : translations) translated_texts << translation.toObject().value("text").toString();
    const auto detected_lang = translations.first().toObject().value("detected_source_language").toString();
    log_info(T("翻译成功: {detected_lang} -> {target_lang}", {{"detected_lang", detected_lang}, {"target_lang", target_lang}}), "DeepL");
    return DeepLBatchResult{true, translated_texts, detected_lang, {}};
}

QString DeepLService::parse_http_error(int code, const QString &reason) {
    static const QHash<int, QString> messages = {
        {400, "Bad request parameters"}, {401, "Invalid API key"}, {403, "API key permission denied"}, {404, "API endpoint not found"},
        {413, "Text too long"}, {429, "Too many requests, please try again later"}, {456, "Quota exceeded"},
        {500, "DeepL server internal error"}, {503, "DeepL service temporarily unavailable"},
    };
    return messages.value(code, QString("HTTP %1: %2").arg(code).arg(reason));
}
}
