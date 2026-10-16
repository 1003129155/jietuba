#pragma once
#include "translation/providers/openai_compatible.h"

namespace jietuba {
class DeepSeekProvider : public OpenAICompatibleProvider {
public:
    // No /v1 suffix: DeepSeek's OpenAI-compatible endpoint sits at the root.
    static inline const QString DEFAULT_BASE_URL = "https://api.deepseek.com";
    static inline const QString DEFAULT_MODEL = "deepseek-flash";
    explicit DeepSeekProvider(const ProviderConfig &config);
    static ProviderMetadata class_metadata();
    QString provider_id() const override { return "deepseek"; }
    QString display_name() const override { return "DeepSeek"; }
};
}
