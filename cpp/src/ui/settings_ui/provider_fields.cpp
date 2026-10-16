#include "core/logger.h"
#include "settings/tool_settings.h"
#include "translation/registry.h"
#include "ui/fluent_lite/switch.h"
#include "ui/settings_ui/pages.h"
#include <QAbstractButton>
#include <QLineEdit>

namespace jietuba::provider_fields {
namespace {
bool is_toggle(const ProviderField &field) { return field.kind == ProviderField::Toggle; }

QVariant empty_value(const ProviderField &field) { return is_toggle(field) ? QVariant(false) : QVariant(QString()); }

bool widget_checked(QWidget *widget) {
    if (auto *button = qobject_cast<SwitchButton *>(widget)) return button->isChecked();
    if (auto *button = qobject_cast<QAbstractButton *>(widget)) return button->isChecked();
    return false;
}

class PendingConfig : public TranslationConfig {
public:
    PendingConfig(ToolSettingsManager *config, QVariantMap values) : config_(config), values_(std::move(values)) {}
    QString get_translation_provider() const override { return config_->get_translation_provider(); }
    QVariantMap get_translation_provider_config(const QString &provider_id) const override {
        auto result = config_->get_translation_provider_config(provider_id);
        for (auto it = values_.begin(); it != values_.end(); ++it) {
            // The longest parameter the key ends with: "amazon_translate_secret_access_key"
            // ends with both "_access_key" and "_secret_access_key".
            QString param;
            for (const auto &key : result.keys())
                if (it.key().endsWith("_" + key) && key.size() > param.size()) param = key;
            if (!param.isEmpty()) result[param] = it.value();
        }
        return result;
    }
    // Quota checks and bookkeeping use the saved settings, as in the reference, where
    // everything but the field getters falls through to the real configuration.
    qint64 get_translation_usage_chars(const QString &provider_id) const override { return config_->get_translation_usage_chars(provider_id); }
    void add_translation_usage_chars(const QString &provider_id, qint64 chars) override { config_->add_translation_usage_chars(provider_id, chars); }
    bool get_translation_quota_enabled(const QString &provider_id) const override { return config_->get_translation_quota_enabled(provider_id); }
    qint64 get_translation_quota_limit(const QString &provider_id) const override { return config_->get_translation_quota_limit(provider_id); }
private:
    ToolSettingsManager *config_;
    QVariantMap values_;
};
}

QVariant read_config(ToolSettingsManager *config, const ProviderField &field) {
    std::optional<QVariant> value;
    try {
        value = config->get_provider_field(field.config_key);
    } catch (const std::exception &e) {
        log_exception(e, T("读取翻译设置 {config_key}", {{"config_key", field.config_key}}));
        return empty_value(field);
    }
    if (!value) return empty_value(field);
    if (is_toggle(field)) return value->toBool();
    return value->toString();
}

void write_config(ToolSettingsManager *config, const ProviderField &field, const QVariant &value) {
    try {
        config->set_provider_field(field.config_key, value);
    } catch (const std::exception &e) {
        log_exception(e, T("保存翻译设置 {config_key}", {{"config_key", field.config_key}}));
    }
}

QVariant widget_value(const ProviderField &field, QWidget *widget) {
    if (is_toggle(field)) return widget_checked(widget);
    // Text is always trimmed: stray spaces in credentials are frequent and invisible.
    auto *edit = qobject_cast<QLineEdit *>(widget);
    return edit ? edit->text().trimmed() : QString();
}

void apply_widget_value(const ProviderField &field, QWidget *widget, const QVariant &value) {
    if (is_toggle(field)) {
        if (auto *button = qobject_cast<SwitchButton *>(widget)) button->setChecked(value.toBool());
        else if (auto *check = qobject_cast<QAbstractButton *>(widget)) check->setChecked(value.toBool());
    } else if (auto *edit = qobject_cast<QLineEdit *>(widget)) {
        edit->setText(value.toString());
    }
}

void load_into(ToolSettingsManager *config, const std::vector<ProviderField> &fields, const QMap<QString, QPointer<QWidget>> &widgets) {
    for (const auto &f : fields)
        if (QWidget *widget = widgets.value(f.config_key)) apply_widget_value(f, widget, read_config(config, f));
}

void save_from(ToolSettingsManager *config, const std::vector<ProviderField> &fields, const QMap<QString, QPointer<QWidget>> &widgets) {
    for (const auto &f : fields)
        if (QWidget *widget = widgets.value(f.config_key)) write_config(config, f, widget_value(f, widget));
}

void reset_to_defaults(const QVariantMap &defaults, const std::vector<ProviderField> &fields, const QMap<QString, QPointer<QWidget>> &widgets) {
    for (const auto &f : fields) {
        QWidget *widget = widgets.value(f.config_key);
        if (!widget || !defaults.contains(f.config_key)) continue;
        apply_widget_value(f, widget, defaults.value(f.config_key));
    }
}

std::vector<ProviderField> provider_fields(const ProviderRegistry &registry, const QString &provider_id) { return registry.metadata(provider_id).all_fields(); }

std::vector<ProviderField> all_fields(const ProviderRegistry &registry) {
    std::vector<ProviderField> fields;
    for (const auto &meta : registry.available_providers()) {
        fields.insert(fields.end(), meta.credentials.begin(), meta.credentials.end());
        fields.insert(fields.end(), meta.options.begin(), meta.options.end());
    }
    return fields;
}

std::shared_ptr<TranslationConfig> pending_config(ToolSettingsManager *config, const std::vector<ProviderField> &fields, const QMap<QString, QPointer<QWidget>> &widgets) {
    QVariantMap values;
    for (const auto &f : fields)
        if (QWidget *widget = widgets.value(f.config_key)) values[f.config_key] = widget_value(f, widget);
    return std::make_shared<PendingConfig>(config, values);
}
}
