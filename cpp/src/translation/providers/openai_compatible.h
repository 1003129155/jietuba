#pragma once
#include "translation/provider.h"
#include "core/http_client.h"
#include <QJsonArray>
#include <QJsonObject>
#include <optional>
#include <utility>

class QJsonValue;

namespace jietuba {

// Translation through an OpenAI-compatible /chat/completions endpoint. Services
// differ only in base URL and model, so subclasses only supply constants.
//
// A general model is asked to act as a translation API and may stop doing so:
// 1. The source text is its own user message, never part of the system prompt,
//    since OCR text often contains instruction-like content.
// 2. JSON output is required so pleasantries cannot pass as the translation.
// 3. The reply also carries detected_source_lang.
// Refusals and residual prompt injection remain inherent to LLM translation.
class OpenAICompatibleProvider : public TranslationProvider {
public:
    struct Defaults {
        QString base_url;
        QString model;
        // Recommended value for translation; nullopt sends none (server default).
        std::optional<double> temperature = 1.0;
        // Sends response_format=json_object; a 400 rejection is retried without it.
        bool json_mode = true;
        // Local services (Ollama, LM Studio) do not check a key.
        bool requires_api_key = true;
        // Floor for the caller's timeout: LLM latency varies far more than a
        // dedicated translation API's, and a 10 s cut shows up as sporadic timeouts.
        int min_timeout = 30;
        // Appended to the error when reasoning used up the whole reply.
        QString thinking_hint;
        // Fixed fields added to the request body; parameter names differ per
        // service and some reject unknown ones, so they are opt-in.
        QJsonObject extra_body;
    };
    static constexpr int MODELS_TIMEOUT = 15;
    OpenAICompatibleProvider(const ProviderConfig &config, const Defaults &defaults);
    bool is_configured() const override;
    TranslationResult translate(const TranslationRequest &request) override;
    QStringList list_models() override;
    QString api_url() const { return base_url_ + "/chat/completions"; }
    // Users often paste the full request URL, which would repeat the path.
    static QString normalize_base_url(QString url);
    // The translation out of a model reply: strips a leading think block and a code
    // fence, and falls back to the whole reply when it is not the requested JSON.
    static std::pair<QString, QString> extract(const QString &content);
    QJsonObject request_body(const TranslationRequest &request, bool json_mode) const;
    TranslationResult parse_response(const QJsonValue &raw) const;
    const QString &config_error() const { return config_error_; }
    std::optional<double> temperature() const { return temperature_; }
    bool json_mode() const { return json_mode_; }
    const QJsonObject &extra_body() const { return extra_body_; }

protected:
    QString api_key_;
    QString base_url_;
    QString model_;
    std::optional<double> temperature_;
    bool json_mode_;
    bool requires_api_key_;
    QString thinking_hint_;
    QJsonObject extra_body_;
    QString instructions_;
    // Invalid advanced settings; not "unconfigured", reported when translating.
    QString config_error_;

private:
    HttpHeaders headers() const;
    QJsonArray build_messages(const TranslationRequest &request) const;
    TranslationResult http_error(const HttpResponse &response) const;
    static TranslationErrorCode map_error_type(const QString &error_type, int status_code);
};
}
