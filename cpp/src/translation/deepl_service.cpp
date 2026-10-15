#include "translation/deepl_service.h"
#include "core/http_client.h"
#include "core/logger.h"
#include "translation/providers/common.h"
#include <QHash>

namespace jietuba {
DeepLResult DeepLService::translate(const QString &text, const QString &target_lang, const std::optional<QString> &source_lang, const QString &split_sentences,
                                    bool preserve_formatting, int timeout) {
    if (text.trimmed().isEmpty()) return DeepLResult{false, {}, {}, "Text is empty"};
    if (api_key.isEmpty()) return DeepLResult{false, {}, {}, "API key not configured"};
    std::vector<std::pair<QString, QString>> data = {
        {"text", text}, {"target_lang", target_lang.toUpper()}, {"split_sentences", split_sentences}, {"preserve_formatting", preserve_formatting ? "1" : "0"}};
    if (source_lang && !source_lang->isEmpty()) data.emplace_back("source_lang", source_lang->toUpper());
    const auto post_data = python_urlencode(data).toUtf8();
    log_debug(T("发送翻译请求: {char_count} 字符 -> {target_lang}", {{"char_count", static_cast<int>(text.toUcs4().size())}, {"target_lang", target_lang}}), "DeepL");
    const auto response = http_request("POST", api_url, {{"Content-Type", "application/x-www-form-urlencoded"}, {"Authorization", "DeepL-Auth-Key " + api_key}},
                                       post_data, timeout);
    if (response.kind == HttpResponse::HttpError) {
        const auto error_msg = parse_http_error(response.status, response.reason);
        log_error(T("HTTP 错误: {error_msg}", {{"error_msg", error_msg}}), "DeepL");
        return DeepLResult{false, {}, {}, error_msg};
    }
    if (response.kind == HttpResponse::Timeout) {
        log_error(T("请求超时（{seconds} 秒）", {{"seconds", timeout}}), "DeepL");
        return DeepLResult{false, {}, {}, QString("Request timed out after %1s").arg(timeout)};
    }
    if (response.kind == HttpResponse::NetworkError) {
        log_error(T("网络错误: {reason}", {{"reason", response.reason}}), "DeepL");
        return DeepLResult{false, {}, {}, "Network error: " + response.reason};
    }
    const auto parsed = parse_json_body(response.body);
    if (!parsed) {
        log_error(T("JSON 解析失败: {e}", {{"e", QString("invalid JSON")}}), "DeepL");
        return DeepLResult{false, {}, {}, "Failed to parse API response"};
    }
    const auto translations = parsed->toObject().value("translations").toArray();
    if (translations.isEmpty()) return DeepLResult{false, {}, {}, "Invalid API response format"};
    const auto translation = translations.first().toObject();
    const auto translated_text = translation.value("text").toString();
    const auto detected_lang = translation.value("detected_source_language").toString();
    log_info(T("翻译成功: {detected_lang} -> {target_lang}", {{"detected_lang", detected_lang}, {"target_lang", target_lang}}), "DeepL");
    return DeepLResult{true, translated_text, detected_lang, {}};
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
