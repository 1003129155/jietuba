#include "core/file_names.h"
#include "core/i18n.h"
#include "settings/tool_settings.h"
#include <QRegularExpression>

namespace jietuba::file_names {
namespace {
const QRegularExpression &extension_pattern() {
    static const QRegularExpression pattern(R"(\.(png|jpe?g|bmp|webp|pdf|gif|mp4)$)", QRegularExpression::CaseInsensitiveOption);
    return pattern;
}
}
QStringList defaults() {
    return {"{time}", "pinned_{time}", i18n_tr("Long Screenshot", "ScrollCaptureWindow") + "_{direction}_{time}",
            "jietuba_gif_{time}", "recording_{time}_{ms}", "original"};
}
QString problem(const QString &pattern, Kind kind) {
    if (pattern.trimmed().isEmpty()) return "Enter a file name rule.";
    if (pattern.size() > 120)
        return "The file name rule is too long (maximum 120 characters).";
    if (render(pattern, "long_horizontal").size() > 120)
        return "The generated file name is too long (maximum 120 characters).";
    if (pattern.contains(QRegularExpression(R"([<>:"/\\|?*\x00-\x1f])")) || pattern.endsWith('.') || pattern.endsWith(' '))
        return "File names cannot contain forbidden characters or end with a dot or space.";
    auto literal = pattern;
    if (kind != OriginalSuffix) {
        literal.replace("{time}", "").replace("{ms}", "");
        if (kind == LongScreenshot) literal.replace("{prefix}", "").replace("{direction}", "");
    }
    if (literal.contains('{') || literal.contains('}')) return "Unknown placeholder in the file name rule.";
    if (kind != OriginalSuffix && pattern.contains(extension_pattern()))
        return "Leave out the file extension; it is added automatically.";
    const auto name = render(pattern, "long");
    if (kind != OriginalSuffix && name.contains(QRegularExpression(R"(^(CON|PRN|AUX|NUL|COM[1-9¹²³]|LPT[1-9¹²³])(\.|$))", QRegularExpression::CaseInsensitiveOption)))
        return "This file name is reserved by Windows.";
    return {};
}
QStringList load(ToolSettingsManager &config) {
    auto result = defaults();
    const auto saved = config.get_save_filename_patterns();
    for (int i = 0; i < result.size() && i < saved.size(); ++i) {
        auto pattern = saved[i];
        if (i == LongScreenshot)
            pattern.replace("{prefix}", i18n_tr("Long Screenshot", "ScrollCaptureWindow") + "_{direction}");
        if (i != OriginalSuffix) pattern.remove(extension_pattern());
        if (problem(pattern, Kind(i)).isEmpty()) result[i] = pattern;
    }
    return result;
}
QString render(const QString &pattern, const QString &prefix, const QDateTime &time) {
    auto name = pattern;
    name.replace("{time}", time.toString("yyyyMMdd_HHmmss"));
    name.replace("{ms}", time.toString("zzz"));
    name.replace("{prefix}", prefix);
    name.replace("{direction}", prefix.section('_', -1));
    return name;
}
QString stem(ToolSettingsManager &config, Kind kind, const QString &prefix) { return render(load(config).at(kind), prefix); }
}
