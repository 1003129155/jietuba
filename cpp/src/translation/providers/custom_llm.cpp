#include "translation/providers/custom_llm.h"
#include "translation/providers/common.h"
#include <cmath>

namespace jietuba {
namespace {
OpenAICompatibleProvider::Defaults custom_defaults() {
    OpenAICompatibleProvider::Defaults defaults;
    defaults.temperature = std::nullopt;
    defaults.requires_api_key = false;
    defaults.min_timeout = CustomLLMProvider::DEFAULT_TIMEOUT;
    // Reasoning models (e.g. on Ollama) think by default and may exhaust the output
    // budget; Ollama's /v1 endpoint accepts reasoning_effort, not think.
    defaults.thinking_hint = "Turn thinking off by adding {\"reasoning_effort\": \"none\"} to Extra Body";
    return defaults;
}
}

CustomLLMProvider::CustomLLMProvider(const ProviderConfig &config) : OpenAICompatibleProvider(config, custom_defaults()) {
    name_ = config.value("name").toString().trimmed();
    json_mode_ = config.value("json_mode", true).toBool();
    instructions_ = config.value("instructions").toString().trimmed();
    QStringList errors;

    const auto temperature = config.value("temperature").toString().trimmed();
    if (!temperature.isEmpty()) {
        bool ok = false;
        const double value = temperature.toDouble(&ok);
        if (ok) temperature_ = value;
        else errors << "Temperature must be a number";
    }

    const auto timeout = config.value("timeout").toString().trimmed();
    if (!timeout.isEmpty()) {
        bool ok = false;
        const double value = timeout.toDouble(&ok);
        if (ok && !std::isnan(value)) min_timeout = static_cast<int>(std::clamp<double>(std::trunc(value), TIMEOUT_MIN, TIMEOUT_MAX));
        else errors << "Timeout must be a number of seconds";
    }

    const auto extra = config.value("extra_body").toString().trimmed();
    if (!extra.isEmpty()) {
        const auto parsed = parse_json_body(extra.toUtf8());
        if (parsed && parsed->isObject()) extra_body_ = parsed->toObject();
        else errors << "Extra Body must be a JSON object";
    }
    config_error_ = errors.join("; ");
}

QString CustomLLMProvider::display_label(const QString &translated_name) const {
    if (!name_.isEmpty()) return name_;
    if (!model_.isEmpty()) return translated_name + QString::fromUtf8(" · ") + model_;
    return translated_name;
}

ProviderMetadata CustomLLMProvider::slot_metadata(const QString &prefix, const QString &display_name) {
    ProviderMetadata metadata;
    metadata.provider_id = prefix;
    metadata.display_name = display_name;
    metadata.credentials = {
        ProviderField::text(prefix + "_base_url", "API Base URL", "https://api.openai.com/v1"),
        ProviderField::text(prefix + "_api_key", "API Key", "Optional for local models", true),
        ProviderField::model(prefix + "_model", "Model", "gpt-4o-mini"),
    };
    metadata.options = {
        ProviderField::text(prefix + "_name", "Display Name", "Shown in the translation window"),
        ProviderField::text(prefix + "_temperature", "Temperature", "Server default"),
        ProviderField::text(prefix + "_timeout", "Timeout (s)", QString::number(DEFAULT_TIMEOUT)),
        ProviderField::text(prefix + "_instructions", "Instructions", "e.g. Keep product names untranslated"),
        ProviderField::text(prefix + "_extra_body", "Extra Body", "{\"reasoning_effort\": \"none\"}"),
        ProviderField::toggle(prefix + "_json_mode", "JSON Mode",
                              "Ask for JSON replies. If the service rejects it, the request is retried without; turn it off to skip that retry"),
    };
    metadata.notice = "Any OpenAI-compatible service. Ollama: http://localhost:11434/v1, LM Studio: http://localhost:1234/v1";
    return metadata;
}
}
