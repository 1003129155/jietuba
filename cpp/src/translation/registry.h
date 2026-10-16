#pragma once
#include "translation/provider.h"
#include <QMap>
#include <functional>
#include <optional>

namespace jietuba {
using ProviderFactory = std::function<std::unique_ptr<TranslationProvider>(const ProviderConfig &)>;

class ProviderRegistry {
public:
    // The provider's own metadata, with an explicit display name taking precedence.
    // Throws std::invalid_argument for an empty id.
    void register_provider(const QString &provider_id, ProviderFactory factory, const std::optional<ProviderMetadata> &metadata = std::nullopt,
                           const QString &display_name = {});
    // Throws std::invalid_argument for an unknown provider.
    std::unique_ptr<TranslationProvider> create(const QString &provider_id, const ProviderConfig &config) const;
    ProviderMetadata metadata(const QString &provider_id) const;
    // In registration order.
    std::vector<ProviderMetadata> available_providers() const;
private:
    QMap<QString, ProviderFactory> factories_;
    QMap<QString, ProviderMetadata> metadata_;
    QStringList order_;
};
}
