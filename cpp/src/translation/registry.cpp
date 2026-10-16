#include "translation/registry.h"
#include <stdexcept>

namespace jietuba {
void ProviderRegistry::register_provider(const QString &provider_id, ProviderFactory factory, const std::optional<ProviderMetadata> &metadata, const QString &display_name) {
    const auto normalized = provider_id.trimmed().toLower();
    if (normalized.isEmpty()) throw std::invalid_argument("provider_id must not be empty");
    factories_[normalized] = std::move(factory);
    // Factories without metadata get the minimum.
    ProviderMetadata built = metadata.value_or(ProviderMetadata{normalized, display_name.isEmpty() ? normalized : display_name, {}, {}, {}, {}, {}, {}});
    built.provider_id = normalized;
    if (!display_name.isEmpty()) built.display_name = display_name;
    if (!metadata_.contains(normalized)) order_.append(normalized);
    metadata_[normalized] = built;
}

std::unique_ptr<TranslationProvider> ProviderRegistry::create(const QString &provider_id, const ProviderConfig &config) const {
    const auto normalized = provider_id.trimmed().toLower();
    if (!factories_.contains(normalized)) throw std::invalid_argument(("Unknown translation provider: " + provider_id).toStdString());
    return factories_[normalized](config);
}

ProviderMetadata ProviderRegistry::metadata(const QString &provider_id) const {
    const auto normalized = provider_id.trimmed().toLower();
    if (!metadata_.contains(normalized)) throw std::invalid_argument(("Unknown translation provider: " + provider_id).toStdString());
    return metadata_[normalized];
}

std::vector<ProviderMetadata> ProviderRegistry::available_providers() const {
    std::vector<ProviderMetadata> result;
    for (const auto &id : order_) result.push_back(metadata_[id]);
    return result;
}
}
