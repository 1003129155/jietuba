#pragma once
#include <QSet>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <memory>
#include <stdexcept>

// Import of the settings the settings window shows, from another settings.ini or a JSON file
// exported by earlier versions. Import fills the window through preview_config; Apply then saves
// through the window's usual flow, like manual edits.
namespace jietuba {
class ToolSettingsManager;

namespace settings_transfer {
// The JSON format earlier versions exported.
inline const QString FILE_TAG = "jietuba-settings";
constexpr int FORMAT_VERSION = 1;
// QSettings keys of the settings an import carries, sorted.
const QStringList &IMPORTED_KEYS();

// The file is not an importable settings file.
class SettingsFileError : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

// Reads and validates a settings file, INI or JSON by its content; any problem throws SettingsFileError.
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
