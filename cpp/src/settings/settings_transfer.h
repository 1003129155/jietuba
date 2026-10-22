#pragma once
#include <QSet>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <memory>
#include <stdexcept>

// Export and import of the settings the settings window shows.
// Export writes the current values of the settings app_settings.inc marks Export as JSON.
// Import fills the window through preview_config; Apply then saves through the window's
// usual flow, like manual edits.
namespace jietuba {
class ToolSettingsManager;

namespace settings_transfer {
inline const QString FILE_TAG = "jietuba-settings";
constexpr int FORMAT_VERSION = 1;
// Keys meaningful only on this machine: never exported or imported.
const QSet<QString> &LOCAL_KEYS();
// QSettings keys of the settings an export carries, sorted.
const QStringList &EXPORTED_KEYS();

// The file is not an importable settings file.
class SettingsFileError : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

// Writes the saved ones among EXPORTED_KEYS to path and returns how many; unsaved ones are
// defaults and are left out. Throws std::runtime_error when the file cannot be written.
int export_settings(ToolSettingsManager *config, const QString &path, const QString &app_version = {});
// Reads and validates a settings file; any problem throws SettingsFileError.
QVariantMap read_settings_file(const QString &path);

// A read-only configuration equal to "after the import": missing keys take defaults, local
// keys keep config's values. The returned object owns its QSettings.
struct PreviewConfig {
    std::unique_ptr<QSettings> settings;
    std::unique_ptr<ToolSettingsManager> config;
};
PreviewConfig preview_config(ToolSettingsManager *config, const QVariantMap &values, const QString &ini_path);
}
}
