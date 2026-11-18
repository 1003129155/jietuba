#pragma once
#include "translation/models.h"
#include <QSet>
#include <QStringList>
#include <QVariantMap>
#include <memory>
#include <vector>

namespace jietuba {
// One setting a provider declares for the settings UI. config_key also names the
// accessors on ToolSettingsManager (get_<config_key> / set_<config_key>), so a new
// provider needs no UI changes; declaring fields here keeps every provider's form
// complete instead of each UI keeping its own list.
struct ProviderField {
    // Text: one line; Model: text plus a button listing models (plain text where
    // unsupported); Toggle: a provider-specific switch.
    enum Kind { Text, Model, Toggle };
    Kind kind = Text;
    QString config_key;
    QString label;
    QString placeholder;
    // Masked, always with a show/hide button.
    bool secret = false;
    QString description;
    static ProviderField text(const QString &key, const QString &label, const QString &placeholder = {}, bool secret = false) {
        return ProviderField{Text, key, label, placeholder, secret, {}};
    }
    static ProviderField model(const QString &key, const QString &label, const QString &placeholder = {}) { return ProviderField{Model, key, label, placeholder, false, {}}; }
    static ProviderField toggle(const QString &key, const QString &label, const QString &description = {}) { return ProviderField{Toggle, key, label, {}, false, description}; }
};

struct ProviderMetadata {
    QString provider_id;
    QString display_name;
    std::vector<ProviderField> credentials;
    std::vector<ProviderField> options;
    // Which "general translation options" this provider uses; for UI visibility only,
    // requests always carry them and other providers ignore them.
    QSet<QString> supported_request_options;
    // The hint under the provider page (free quota and the like).
    QString notice;
    QString help_label;
    QString help_url;
    // Credentials plus provider options: everything the provider's page shows.
    std::vector<ProviderField> all_fields() const;
};

using ProviderConfig = QVariantMap;

class TranslationProvider {
public:
    virtual ~TranslationProvider() = default;
    virtual QString provider_id() const = 0;
    virtual QString display_name() const = 0;
    virtual bool is_configured() const = 0;
    // One synchronous request.
    virtual TranslationResult translate(const TranslationRequest &request) = 0;
    // Whether translate_batch() is implemented. Dedicated translation APIs already answer
    // a whole text in one call, so they leave this false; LLM endpoints, where the cost is
    // the round trip plus the model thinking, override it.
    virtual bool supports_batch() const { return false; }
    // Several texts in one request, in the same order. request carries the target
    // language and the options; its text is ignored. A failure makes the caller fall
    // back to translate() one text at a time, so nothing is lost if the model breaks
    // the array contract.
    virtual BatchTranslationResult translate_batch(const QStringList &texts, const TranslationRequest &request);
    int effective_timeout(const TranslationRequest &request) const { return std::max(request.timeout, min_timeout); }
    virtual std::optional<QSet<QString>> supported_target_languages() const { return std::nullopt; }
    // The name shown in the translation window; translated_name is display_name
    // translated for the UI, since translation contexts live in the UI layer.
    virtual QString display_label(const QString &translated_name) const { return translated_name; }
    // Model names the service offers; only providers declaring a model field implement
    // it. Throws std::runtime_error with a message for the user.
    virtual QStringList list_models();
    int min_timeout = 0;
    // Monthly free quota in characters that the service guards; 0 for none.
    qint64 quota_limit_chars = 0;
};
}
