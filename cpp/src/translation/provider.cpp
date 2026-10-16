#include "translation/provider.h"
#include <stdexcept>

namespace jietuba {
std::vector<ProviderField> ProviderMetadata::all_fields() const {
    auto fields = credentials;
    fields.insert(fields.end(), options.begin(), options.end());
    return fields;
}

QStringList TranslationProvider::list_models() { throw std::logic_error("list_models not implemented"); }
}
