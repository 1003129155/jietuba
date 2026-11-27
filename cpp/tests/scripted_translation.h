#pragma once
#include "translation/registry.h"
#include "translation/service.h"
#include <functional>
#include <memory>

// A translation service whose answer to each text the test decides. It answers on the thread
// that asks, the worker's for an overlay translation.
using TranslationScript = std::function<jietuba::TranslationResult(const QString &)>;

class ScriptedProvider : public jietuba::TranslationProvider {
public:
    explicit ScriptedProvider(TranslationScript script) : script_(std::move(script)) { parallel_requests = 2; }
    QString provider_id() const override { return "scripted"; }
    QString display_name() const override { return "Scripted"; }
    bool is_configured() const override { return true; }
    jietuba::TranslationResult translate(const jietuba::TranslationRequest &request) override { return script_(request.text); }
private:
    TranslationScript script_;
};

class ScriptedConfig : public jietuba::TranslationConfig {
public:
    QString get_translation_provider() const override { return "scripted"; }
    QVariantMap get_translation_provider_config(const QString &) const override { return {}; }
};

inline std::shared_ptr<jietuba::TranslationService> scripted_service(const TranslationScript &script) {
    auto registry = std::make_shared<jietuba::ProviderRegistry>();
    registry->register_provider("scripted", [script](const jietuba::ProviderConfig &) { return std::make_unique<ScriptedProvider>(script); }, std::nullopt, "Scripted");
    return std::make_shared<jietuba::TranslationService>(registry, std::make_shared<ScriptedConfig>());
}

inline jietuba::TranslationResult translated_as(const QString &text) { return jietuba::TranslationResult{true, text, {}, std::nullopt, {}}; }
