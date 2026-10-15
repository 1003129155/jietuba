#pragma once
#include "translation/providers/openai_compatible.h"

namespace jietuba {
// Any OpenAI-compatible service. Nothing is preset: base URL and model are required,
// the key is optional (local services do not check it), no temperature is sent
// unless set (some models reject anything but the default), and service-specific
// parameters go into the user's extra body.
class CustomLLMProvider : public OpenAICompatibleProvider {
public:
    // Local models on CPU often take over a minute on long text.
    static constexpr int DEFAULT_TIMEOUT = 60;
    static constexpr int TIMEOUT_MIN = 5, TIMEOUT_MAX = 600;
    explicit CustomLLMProvider(const ProviderConfig &config);
    // Slot 1 keeps the single-slot provider id and setting keys, so existing
    // configurations and engine selections need no migration.
    static ProviderMetadata class_metadata() { return slot_metadata("custom_llm", "Custom LLM-1"); }
    QString provider_id() const override { return "custom_llm"; }
    QString display_name() const override { return "Custom LLM-1"; }
    QString display_label(const QString &translated_name) const override;

protected:
    static ProviderMetadata slot_metadata(const QString &prefix, const QString &display_name);

private:
    QString name_;
};

// Further slots, identical to the first apart from id and setting keys.
template <int Slot>
class CustomLLMSlotProvider : public CustomLLMProvider {
public:
    using CustomLLMProvider::CustomLLMProvider;
    static ProviderMetadata class_metadata() { return slot_metadata(prefix(), QString("Custom LLM-%1").arg(Slot)); }
    QString provider_id() const override { return prefix(); }
    QString display_name() const override { return QString("Custom LLM-%1").arg(Slot); }
private:
    static QString prefix() { return QString("custom_llm_%1").arg(Slot); }
};

using CustomLLM2Provider = CustomLLMSlotProvider<2>;
using CustomLLM3Provider = CustomLLMSlotProvider<3>;
}
