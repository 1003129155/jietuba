#include "translation/service.h"
#include "core/logger.h"
#include "core/py_text.h"
#include "settings/tool_settings.h"
#include "translation/providers/amazon.h"
#include "translation/providers/azure.h"
#include "translation/providers/baidu.h"
#include "translation/providers/custom_llm.h"
#include "translation/providers/deepl.h"
#include "translation/providers/deepseek.h"
#include "translation/providers/google.h"
#include "translation/providers/tencent.h"
#include "translation/providers/volcengine.h"
#include "translation/providers/youdao.h"
#include <stdexcept>

namespace jietuba {
namespace {
// The settings manager as translation configuration.
class SettingsTranslationConfig : public TranslationConfig {
public:
    explicit SettingsTranslationConfig(ToolSettingsManager *manager) : manager_(manager) {}
    QString get_translation_provider() const override { return manager_->get_translation_provider(); }
    QVariantMap get_translation_provider_config(const QString &provider_id) const override { return manager_->get_translation_provider_config(provider_id); }
    qint64 get_translation_usage_chars(const QString &provider_id) const override { return manager_->get_translation_usage_chars(provider_id); }
    void add_translation_usage_chars(const QString &provider_id, qint64 chars) override { manager_->add_translation_usage_chars(provider_id, chars); }
    bool get_translation_quota_enabled(const QString &provider_id) const override { return manager_->get_translation_quota_enabled(provider_id); }
    qint64 get_translation_quota_limit(const QString &provider_id) const override { return manager_->get_translation_quota_limit(provider_id); }
private:
    ToolSettingsManager *manager_;
};

template <class Provider>
void register_builtin(ProviderRegistry &registry) {
    const auto metadata = Provider::class_metadata();
    registry.register_provider(metadata.provider_id, [](const ProviderConfig &config) { return std::make_unique<Provider>(config); }, metadata, metadata.display_name);
}
}

QString TranslationService::active_provider_id() const { return config->get_translation_provider().trimmed().toLower(); }

std::unique_ptr<TranslationProvider> TranslationService::provider(const QString &provider_id, const QVariantMap &overrides) const {
    const auto selected = (provider_id.isEmpty() ? active_provider_id() : provider_id).trimmed().toLower();
    auto provider_config = config->get_translation_provider_config(selected);
    for (auto it = overrides.begin(); it != overrides.end(); ++it) provider_config[it.key()] = it.value();
    return registry->create(selected, provider_config);
}

QString TranslationService::provider_name(const QString &provider_id) const {
    return registry->metadata(provider_id.isEmpty() ? active_provider_id() : provider_id).display_name;
}

QString TranslationService::provider_label(const std::function<QString(const QString &)> &translate, const QString &provider_id, const QVariantMap &overrides) const {
    const auto raw = provider_name(provider_id);
    const auto name = translate ? translate(raw) : raw;
    return provider(provider_id, overrides)->display_label(name);
}

bool TranslationService::is_configured(const QString &provider_id, const QVariantMap &overrides) const {
    try {
        return provider(provider_id, overrides)->is_configured();
    } catch (const std::invalid_argument &) {
        return false;
    }
}

TranslationResult TranslationService::translate(const TranslationRequest &request, const QString &provider_id, const QVariantMap &overrides) const {
    std::unique_ptr<TranslationProvider> selected;
    try {
        selected = provider(provider_id, overrides);
    } catch (const std::invalid_argument &e) {
        return TranslationResult::failure(TranslationErrorCode::NOT_CONFIGURED, QString::fromUtf8(e.what()));
    }
    if (!selected->is_configured()) return TranslationResult::failure(TranslationErrorCode::NOT_CONFIGURED, selected->display_name() + " is not configured");
    if (auto quota_error = check_quota(*selected)) return *quota_error;
    auto result = selected->translate(request);
    if (result.success) record_usage(*selected, py_len(request.text));
    return result;
}

std::optional<TranslationResult> TranslationService::check_quota(const TranslationProvider &provider) const {
    const auto limit = effective_quota_limit(provider);
    if (!limit) return std::nullopt;
    const auto used = config->get_translation_usage_chars(provider.provider_id());
    if (used < *limit) return std::nullopt;
    log_info(QString("%1 hit monthly quota %2/%3").arg(provider.display_name()).arg(used).arg(*limit), "TranslationService");
    return TranslationResult::failure(TranslationErrorCode::QUOTA_EXCEEDED,
                                      QString("%1 monthly quota reached (%2/%3 characters), resets next month").arg(provider.display_name()).arg(used).arg(*limit));
}

std::optional<qint64> TranslationService::effective_quota_limit(const TranslationProvider &provider) const {
    if (provider.quota_limit_chars <= 0) return std::nullopt;
    const auto provider_id = provider.provider_id();
    if (!config->get_translation_quota_enabled(provider_id)) return std::nullopt;
    const auto custom = config->get_translation_quota_limit(provider_id);
    return custom > 0 ? custom : provider.quota_limit_chars;
}

void TranslationService::record_usage(const TranslationProvider &provider, qint64 chars) const {
    if (provider.quota_limit_chars <= 0) return;
    config->add_translation_usage_chars(provider.provider_id(), chars);
}

std::shared_ptr<TranslationService> create_default_translation_service(std::shared_ptr<TranslationConfig> config) {
    if (!config) config = std::make_shared<SettingsTranslationConfig>(get_tool_settings_manager());
    auto registry = std::make_shared<ProviderRegistry>();
    register_builtin<DeepLProvider>(*registry);
    register_builtin<AmazonTranslateProvider>(*registry);
    register_builtin<GoogleTranslateProvider>(*registry);
    register_builtin<AzureTranslateProvider>(*registry);
    register_builtin<BaiduTranslateProvider>(*registry);
    register_builtin<TencentTranslateProvider>(*registry);
    register_builtin<VolcengineTranslateProvider>(*registry);
    register_builtin<YoudaoTranslateProvider>(*registry);
    register_builtin<DeepSeekProvider>(*registry);
    register_builtin<CustomLLMProvider>(*registry);
    register_builtin<CustomLLM2Provider>(*registry);
    register_builtin<CustomLLM3Provider>(*registry);
    return std::make_shared<TranslationService>(registry, config);
}
}
