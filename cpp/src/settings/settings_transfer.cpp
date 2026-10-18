#include "settings/settings_transfer.h"
#include "settings/tool_settings.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <algorithm>

namespace jietuba::settings_transfer {
namespace {
// JSON-compatible plain values; other types become their string form.
QJsonValue plain(const QVariant &value) {
    if (!value.isValid() || value.isNull()) return QJsonValue::Null;
    switch (value.typeId()) {
    case QMetaType::QString:
        return value.toString();
    case QMetaType::Bool:
        return value.toBool();
    case QMetaType::Int:
    case QMetaType::LongLong:
    case QMetaType::UInt:
    case QMetaType::ULongLong:
        return value.toLongLong();
    case QMetaType::Double:
    case QMetaType::Float:
        return value.toDouble();
    case QMetaType::QStringList:
    case QMetaType::QVariantList: {
        QJsonArray array;
        for (const auto &item : value.toList()) array.append(plain(item));
        return array;
    }
    default:
        return value.toString();
    }
}
}

const QSet<QString> &LOCAL_KEYS() {
    static const QSet<QString> keys{
        "app/has_run_before",
        "app/welcome_wizard_done",
        // Autostart is decided by the system Run entry and scheduled task; these values alone would disagree with them.
        "app/autostart_enabled",
        "app/run_as_admin",
        // Absolute paths seldom exist on another computer.
        "app/screenshot_save_path",
        "app/log_dir",
        "clipboard/db_path",
    };
    return keys;
}

RecordingReads::RecordingReads(ToolSettingsManager *config) : config_(config), previous_(config->qsettings.recorder) { config_->qsettings.recorder = &keys; }

RecordingReads::~RecordingReads() { config_->qsettings.recorder = previous_; }

int export_settings(ToolSettingsManager *config, const QSet<QString> &keys, const QString &path, const QString &app_version) {
    auto &qsettings = config->qsettings;
    qsettings.sync();
    QStringList sorted = keys.values();
    std::sort(sorted.begin(), sorted.end());
    QJsonObject values;
    for (const auto &key : sorted)
        if (!LOCAL_KEYS().contains(key) && qsettings.contains(key)) values[key] = plain(qsettings.value(key));
    const QJsonObject payload{{"format", FILE_TAG}, {"version", FORMAT_VERSION}, {"app_version", app_version}, {"settings", values}};
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) throw std::runtime_error(file.errorString().toStdString());
    if (file.write(QJsonDocument(payload).toJson(QJsonDocument::Indented)) < 0) throw std::runtime_error(file.errorString().toStdString());
    return static_cast<int>(values.size());
}

QVariantMap read_settings_file(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) throw SettingsFileError(file.errorString().toStdString());
    QByteArray data = file.readAll();
    if (data.startsWith("\xEF\xBB\xBF")) data.remove(0, 3);
    QJsonParseError error{};
    const auto document = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError) throw SettingsFileError(error.errorString().toStdString());
    const auto payload = document.object();
    if (!document.isObject() || payload.value("format") != FILE_TAG || !payload.value("settings").isObject()) throw SettingsFileError("not a Jietuba settings file");
    const auto version = payload.value("version");
    const double number = version.toDouble();
    if (!version.isDouble() || number != static_cast<qint64>(number) || number > FORMAT_VERSION)
        throw SettingsFileError(QString("unsupported format version: %1").arg(QString::fromUtf8(QJsonDocument(QJsonArray{version}).toJson(QJsonDocument::Compact))).toStdString());
    QVariantMap result;
    const auto settings = payload.value("settings").toObject();
    for (auto it = settings.begin(); it != settings.end(); ++it) {
        const auto value = it.value();
        if (it.key().isEmpty() || LOCAL_KEYS().contains(it.key())) continue;
        if (value.isString() || value.isDouble() || value.isBool() || value.isArray()) result[it.key()] = value.toVariant();
    }
    return result;
}

PreviewConfig preview_config(ToolSettingsManager *config, const QVariantMap &values, const QString &ini_path) {
    PreviewConfig preview;
    preview.settings = std::make_unique<QSettings>(ini_path, QSettings::IniFormat);
    preview.settings->clear();
    for (const auto &key : LOCAL_KEYS())
        if (config->qsettings.contains(key)) preview.settings->setValue(key, config->qsettings.value(key));
    for (auto it = values.begin(); it != values.end(); ++it) preview.settings->setValue(it.key(), it.value());
    preview.config = std::make_unique<ToolSettingsManager>(*preview.settings);
    return preview;
}
}
