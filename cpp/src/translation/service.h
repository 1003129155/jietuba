#pragma once
#include "translation/registry.h"
#include <functional>
#include <memory>

namespace jietuba {
class ToolSettingsManager;

// What the service reads from configuration.
class TranslationConfig {
public:
    virtual ~TranslationConfig() = default;
    virtual QString get_translation_provider() const = 0;
    virtual QVariantMap get_translation_provider_config(const QString &provider_id) const = 0;
    // Monthly quota bookkeeping. Only providers declaring quota_limit_chars reach these,
    // so a configuration without quota storage keeps the neutral defaults.
    virtual qint64 get_translation_usage_chars(const QString &) const { return 0; }
    virtual void add_translation_usage_chars(const QString &, qint64) {}
    virtual bool get_translation_quota_enabled(const QString &) const { return true; }
    virtual qint64 get_translation_quota_limit(const QString &) const { return 0; }
};

// Provider selection and translation orchestration.
class TranslationService {
public:
    TranslationService(std::shared_ptr<ProviderRegistry> registry, std::shared_ptr<TranslationConfig> config) : registry(std::move(registry)), config(std::move(config)) {}
    QString active_provider_id() const;
    // Throws std::invalid_argument for an unknown provider.
    std::unique_ptr<TranslationProvider> provider(const QString &provider_id = {}, const QVariantMap &overrides = {}) const;
    QString provider_name(const QString &provider_id = {}) const;
    // The service name in the translation window. Custom services may use the user's
    // name, so the provider is built from the current configuration and asked.
    QString provider_label(const std::function<QString(const QString &)> &translate = {}, const QString &provider_id = {}, const QVariantMap &overrides = {}) const;
    bool is_configured(const QString &provider_id = {}, const QVariantMap &overrides = {}) const;
    TranslationResult translate(const TranslationRequest &request, const QString &provider_id = {}, const QVariantMap &overrides = {}) const;
    // QUOTA_EXCEEDED once this month's usage reaches the limit, else nullopt. Reads
    // QSettings, so GUI thread only.
    std::optional<TranslationResult> check_quota(const TranslationProvider &provider) const;
    // Counts toward this month's usage, also while the guard is off so the figure stays
    // right when it is switched back on.
    void record_usage(const TranslationProvider &provider, qint64 chars) const;
    std::shared_ptr<ProviderRegistry> registry;
    std::shared_ptr<TranslationConfig> config;
private:
    // nullopt without a declared quota or with the guard off.
    std::optional<qint64> effective_quota_limit(const TranslationProvider &provider) const;
};

// The service with every built-in provider registered; config defaults to the settings manager.
std::shared_ptr<TranslationService> create_default_translation_service(std::shared_ptr<TranslationConfig> config = nullptr);
}
