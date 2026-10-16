#pragma once
#include "translation/provider.h"
#include "translation/service.h"
#include "ui/fluent_lite/theme.h"
#include <QColor>
#include <QMap>
#include <QPointer>
#include <QString>
#include <QVariantMap>
#include <QWidget>
#include <memory>
#include <optional>
#include <set>
#include <tuple>
#include <vector>

// Settings page builders, by content stack index (see SettingsDialog).
namespace jietuba {
class HotkeyEdit;
class ProviderRegistry;
class SettingsDialog;
class ToolSettingsManager;

QWidget *create_hotkey_page(SettingsDialog *dialog);
QWidget *create_capture_page(SettingsDialog *dialog);
QWidget *create_clipboard_page(SettingsDialog *dialog);
QWidget *create_appearance_page(SettingsDialog *dialog);
QWidget *create_translation_page(SettingsDialog *dialog);
QWidget *create_log_page(SettingsDialog *dialog);
QWidget *create_misc_page(SettingsDialog *dialog);
QWidget *create_developer_page(SettingsDialog *dialog);
QWidget *create_about_page(SettingsDialog *dialog);
QWidget *create_quick_actions_page(SettingsDialog *dialog);
QWidget *create_mouse_page(SettingsDialog *dialog);
QWidget *create_pin_page(SettingsDialog *dialog);

// page_hotkey: the global hotkey editors form one conflict domain.
extern const QStringList GLOBAL_HOTKEY_EDIT_ATTRS;
// The editor attribute names of a function's slots in slot order; slot n adds _n, like the setting.
QStringList global_hotkey_edit_attrs(const QString &first_attr);
// (function, editor of each slot); the editors are null until the hotkey page is built.
std::vector<std::pair<QString, std::vector<HotkeyEdit *>>> global_hotkey_edit_rows(SettingsDialog *dialog);
bool validate_global_hotkey_edits(SettingsDialog *dialog, bool check_system = false);
QString inapp_label(SettingsDialog *dialog, const QString &cfg_key);
// (icon, title, default) of each in-app shortcut, as the hotkey page lists them.
struct InAppKey {
    QString cfg_key;
    const char *label;
    QString default_value;
};
const std::vector<InAppKey> &INAPP_KEYS();
// Row icons of in-app shortcuts: an svg/ file name or a FluentIcon.
IconSource inapp_icon(const QString &cfg_key);

// page_mouse: every duplicate mouse binding as (scope, binding, owners).
std::vector<std::tuple<QString, QString, QStringList>> mouse_binding_conflicts(SettingsDialog *dialog);
QString mouse_binding_conflict_message(SettingsDialog *dialog, const std::optional<std::vector<std::tuple<QString, QString, QStringList>>> &conflicts = std::nullopt);

// page_pin: settings the pin page saves through behavior_controls, restored by its reset.
extern const std::set<QString> PIN_BEHAVIOR_KEYS;

// page_appearance: fills a swatch button with a colour.
void update_color_btn(QWidget *btn, const QColor &color);
// page_appearance: fills a swatch button with a clipboard theme's two colours.
void apply_clip_theme_btn_style(QWidget *btn, const QString &name);

// page_clipboard: the database and images size as text, empty when unavailable. Scans the disk.
QString clipboard_storage_size_text();

// page_log
void refresh_latest_log_label(SettingsDialog *dialog);

// page_translation: this month's usage and the guard state of a provider declaring a free
// quota; empty for other providers.
QString quota_usage_text(SettingsDialog *dialog, const QString &provider_id);

// provider_fields: reading and writing provider settings declared by Field; no layout.
// The settings page and the welcome page lay them out differently, but which fields exist
// and how they are stored is the same, so neither UI can miss one.
namespace provider_fields {
// The field's value from the configuration; the type's empty value when unavailable.
QVariant read_config(ToolSettingsManager *config, const ProviderField &field);
// Writes a field back; skipped without a setter.
void write_config(ToolSettingsManager *config, const ProviderField &field, const QVariant &value);
QVariant widget_value(const ProviderField &field, QWidget *widget);
void apply_widget_value(const ProviderField &field, QWidget *widget, const QVariant &value);
void load_into(ToolSettingsManager *config, const std::vector<ProviderField> &fields, const QMap<QString, QPointer<QWidget>> &widgets);
void save_from(ToolSettingsManager *config, const std::vector<ProviderField> &fields, const QMap<QString, QPointer<QWidget>> &widgets);
// Restores defaults; keys missing from defaults are skipped rather than cleared, since
// clearing would erase credentials the user entered.
void reset_to_defaults(const QVariantMap &defaults, const std::vector<ProviderField> &fields, const QMap<QString, QPointer<QWidget>> &widgets);
// One provider's fields (credentials and its own options).
std::vector<ProviderField> provider_fields(const ProviderRegistry &registry, const QString &provider_id);
// Every registered provider's fields, in registration order.
std::vector<ProviderField> all_fields(const ProviderRegistry &registry);
// Unsaved form values over the real configuration, for "Test Connection" and "Fetch Models".
// Each field maps to the provider parameter its config key ends with, so the provider
// parameters come from the same get_translation_provider_config as real translations.
std::shared_ptr<TranslationConfig> pending_config(ToolSettingsManager *config, const std::vector<ProviderField> &fields, const QMap<QString, QPointer<QWidget>> &widgets);
}
}
