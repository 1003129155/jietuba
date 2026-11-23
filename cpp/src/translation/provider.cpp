#include "translation/provider.h"
#include <QString>
#include <algorithm>
#include <stdexcept>

namespace jietuba {
namespace {
// The number a setting holds, clamped; fallback for a blank or non-numeric one.
int pacing_value(const QVariant &text, int fallback, int low, int high) {
    // NFKC turns full-width digits typed through an IME into ASCII ones.
    const auto digits = text.toString().normalized(QString::NormalizationForm_KC).trimmed();
    bool ok = false;
    const int value = digits.toInt(&ok);
    return ok ? std::clamp(value, low, high) : fallback;
}
}

std::vector<ProviderField> pacing_fields(const QString &prefix, const RequestPacing &defaults) {
    return {
        ProviderField::text(prefix + "_parallel_requests", "Parallel requests", QString::number(defaults.parallel)).as_advanced(),
        ProviderField::text(prefix + "_request_interval_ms", "Interval (ms)", QString::number(defaults.interval_ms)).as_advanced(),
    };
}

std::vector<ProviderField> ProviderMetadata::all_fields() const {
    auto fields = credentials;
    fields.insert(fields.end(), options.begin(), options.end());
    fields.insert(fields.end(), pacing.begin(), pacing.end());
    return fields;
}

void TranslationProvider::apply_pacing(const RequestPacing &defaults, const QVariantMap &config) {
    parallel_requests = pacing_value(config.value("parallel_requests"), defaults.parallel, 1, RequestPacing::MAX_PARALLEL);
    request_interval_ms = pacing_value(config.value("request_interval_ms"), defaults.interval_ms, 0, RequestPacing::MAX_INTERVAL_MS);
}

QStringList TranslationProvider::list_models() { throw std::logic_error("list_models not implemented"); }

BatchTranslationResult TranslationProvider::translate_batch(const QStringList &, const TranslationRequest &) {
    return BatchTranslationResult::failure(TranslationErrorCode::UNKNOWN, "Batch translation is not supported by this engine");
}
}
