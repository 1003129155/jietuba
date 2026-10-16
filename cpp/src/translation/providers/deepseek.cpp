#include "translation/providers/deepseek.h"

namespace jietuba {
namespace {
OpenAICompatibleProvider::Defaults deepseek_defaults() {
    OpenAICompatibleProvider::Defaults defaults;
    defaults.base_url = DeepSeekProvider::DEFAULT_BASE_URL;
    defaults.model = DeepSeekProvider::DEFAULT_MODEL;
    // DeepSeek's documented recommendation for translation; the output format is
    // held by JSON mode, not by a low temperature.
    defaults.temperature = 1.3;
    // Thinking is on by default and adds latency and tokens translation does not
    // need. "thinking" is used because reasoning_effort documents no "none" value.
    defaults.extra_body = QJsonObject{{"thinking", QJsonObject{{"type", "disabled"}}}};
    return defaults;
}
}

DeepSeekProvider::DeepSeekProvider(const ProviderConfig &config) : OpenAICompatibleProvider(config, deepseek_defaults()) {}

ProviderMetadata DeepSeekProvider::class_metadata() {
    ProviderMetadata metadata;
    metadata.provider_id = "deepseek";
    metadata.display_name = "DeepSeek";
    metadata.credentials = {
        ProviderField::text("deepseek_api_key", "DeepSeek API Key", "sk-...", true),
        // Model names do change between generations; editable so a rename needs no release.
        ProviderField::model("deepseek_model", "Model", DEFAULT_MODEL),
        // Normally untouched; allows a proxy, mirror or any other compatible service.
        ProviderField::text("deepseek_base_url", "API Base URL", DEFAULT_BASE_URL),
    };
    metadata.notice = "LLM translation: better with context and idioms, but slower, billed per token, and may refuse some content";
    metadata.help_label = "platform.deepseek.com";
    metadata.help_url = "https://platform.deepseek.com/api_keys";
    return metadata;
}
}
