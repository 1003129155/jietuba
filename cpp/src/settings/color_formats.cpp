#include "settings/color_formats.h"
#include "settings/tool_settings.h"
#include "core/ui_scale.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>

namespace jietuba::color_formats {
namespace {
const QVector<ColorFormat> builtins{
    {"RGB", "{r}, {g}, {b}", false}, {"CSS rgb()", "rgb({r}, {g}, {b})", false},
    {"HEX", "#{hex}", false}, {"HEX without #", "{hex}", false},
    {"CSS hsl()", "hsl({h}, {s}%, {l}%)", false}};
bool truth(const QJsonValue &v) {
    if (v.isBool()) return v.toBool();
    if (v.isDouble()) return v.toDouble() != 0;
    if (v.isString()) return !v.toString().isEmpty();
    if (v.isArray()) return !v.toArray().isEmpty();
    if (v.isObject()) return !v.toObject().isEmpty();
    return false;
}
}
QString ColorFormat::render(const QColor &c) const {
    auto output = template_string;
    output.replace("{r}", QString::number(c.red()));
    output.replace("{g}", QString::number(c.green()));
    output.replace("{b}", QString::number(c.blue()));
    output.replace("{hex}", QString("%1%2%3").arg(c.red(), 2, 16, QChar('0')).arg(c.green(), 2, 16, QChar('0')).arg(c.blue(), 2, 16, QChar('0')).toUpper());
    output.replace("{h}", QString::number(std::max(c.hslHue(), 0)));
    output.replace("{s}", QString::number(python_round(c.hslSaturationF() * 100)));
    output.replace("{l}", QString::number(python_round(c.lightnessF() * 100)));
    return output;
}
QVector<ColorFormat> default_formats() {
    auto result = builtins;
    for (auto &format : result) format.enabled = format.name == "RGB" || format.name == "HEX";
    return result;
}
QVector<ColorFormat> normalize(const QVector<ColorFormat> &formats) {
    QVector<ColorFormat> result;
    QSet<QString> seen;
    for (const auto &format : formats) for (const auto &builtin : builtins) {
        if (format.name == builtin.name && !seen.contains(format.name)) {
            seen.insert(format.name);
            result.push_back({format.name, builtin.template_string, format.enabled});
            break;
        }
    }
    for (const auto &builtin : builtins) if (!seen.contains(builtin.name)) result.push_back(builtin);
    if (std::none_of(result.begin(), result.end(), [](const auto &format) { return format.enabled; })) result[0].enabled = true;
    return result;
}
QVector<ColorFormat> enabled_formats(const QVector<ColorFormat> &formats) {
    QVector<ColorFormat> result;
    for (const auto &format : formats) if (format.enabled) result.push_back(format);
    return result;
}
QVector<ColorFormat> migrate_from_legacy(const ToolSettingsManager &config) {
    static const QMap<QString, QString> names{{"rgb", "RGB"}, {"rgb_css", "CSS rgb()"},
        {"hex", "HEX"}, {"hex_bare", "HEX without #"}, {"hsl_css", "CSS hsl()"}};
    const auto wanted = names.value(config.get_magnifier_color_copy_format());
    if (wanted.isEmpty()) return default_formats();
    auto formats = builtins;
    for (auto &format : formats) format.enabled = format.name == wanted;
    return normalize(formats);
}
QVector<ColorFormat> load(const ToolSettingsManager *config) {
    if (!config) return default_formats();
    const auto raw = config->get_magnifier_color_formats();
    if (!raw.isEmpty()) {
        const auto items = QJsonDocument::fromJson(raw.toUtf8()).array();
        QVector<ColorFormat> formats;
        for (const auto &v : items) if (v.isObject()) {
            const auto item = v.toObject();
            formats.push_back({item["name"].toString(), {}, truth(item["enabled"])});
        }
        if (!formats.isEmpty()) return normalize(formats);
    }
    return migrate_from_legacy(*config);
}
void save(ToolSettingsManager *config, const QVector<ColorFormat> &formats) {
    if (!config) return;
    QJsonArray payload;
    for (const auto &f : normalize(formats)) payload.append(QJsonObject{{"name", f.name}, {"enabled", f.enabled}});
    // Qt sorts object keys. Preserve Python's insertion order in persisted JSON.
    QStringList rows;
    for (const auto &v : payload) {
        const auto item = v.toObject();
        const auto quoted = QJsonDocument(QJsonArray{item["name"]}).toJson(QJsonDocument::Compact);
        rows.append("{\"name\": " + QString::fromUtf8(quoted.mid(1, quoted.size() - 2)) +
                    ", \"enabled\": " + (item["enabled"].toBool() ? "true" : "false") + "}");
    }
    config->set_magnifier_color_formats("[" + rows.join(", ") + "]");
}
}
