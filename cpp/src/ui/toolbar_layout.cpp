#include "ui/toolbar_layout.h"
#include "settings/tool_settings.h"
#include <QJsonDocument>
#include <QSet>

namespace jietuba::toolbar_layout {
namespace {
// Qt sorts object keys, whereas Python's JSON decoder retains insertion order.
// Keep original member slices for Python's two-key dictionary unpacking case.
QVector<QByteArray> members(QByteArray data) {
    data = data.trimmed();
    QVector<QByteArray> result;
    if (data.size() < 2) return result;
    int depth = 0, start = 1;
    bool quoted = false, escaped = false;
    for (int i = 1; i < data.size() - 1; ++i) {
        const char c = data[i];
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else {
            if (c == '"') quoted = true;
            else if (c == '[' || c == '{') ++depth;
            else if (c == ']' || c == '}') --depth;
            else if (c == ',' && depth == 0) { result.append(data.mid(start, i - start)); start = i + 1; }
        }
    }
    const auto last = data.mid(start, data.size() - 1 - start).trimmed();
    if (!last.isEmpty()) result.append(last);
    return result;
}
QString object_key(QByteArray member) {
    member = member.trimmed();
    bool escaped = false;
    for (int i = 1; i < member.size(); ++i) {
        if (escaped) escaped = false;
        else if (member[i] == '\\') escaped = true;
        else if (member[i] == '"') {
            const auto array = QJsonDocument::fromJson("[" + member.left(i + 1) + "]").array();
            return array.isEmpty() ? QString() : array.first().toString();
        }
    }
    return {};
}
}
const QStringList &default_order() {
    static const QStringList order{
        "long_screenshot", "save", "screenshot_translate", "text_recognize", "scan_code", "gif",
        "pen", "highlighter", "mosaic", "spotlight", "watermark", "arrow", "number", "rect", "ellipse", "text", "eraser",
        "undo", "redo", "cancel", "pin", "confirm"};
    return order;
}
// Later, rarely used features are folded by default so the default toolbar does not keep growing.
const LayoutProfile SCREENSHOT{default_order(), {"text_recognize", "scan_code", "spotlight", "watermark"}, {"confirm"}, SETTING_KEY};
const LayoutProfile PIN{
    {"save", "copy",
     "pen", "highlighter", "mosaic", "spotlight", "watermark", "arrow", "number", "rect", "ellipse", "text",
     "eraser",
     "undo", "redo"},
    {"spotlight", "watermark"}, {}, "pin_toolbar_layout"};
const LayoutProfile ANNOTATION{
    {"annotation_pointer",
     "pen", "highlighter", "arrow", "number", "rect", "ellipse", "text", "eraser",
     "annotation_board", "annotation_cursor",
     "undo", "redo", "annotation_clear", "annotation_hide", "annotation_exit"},
    {}, {"annotation_exit"}, "annotation_toolbar_layout"};
const QStringList &setting_keys() {
    static const QStringList keys{SCREENSHOT.setting_key, PIN.setting_key, ANNOTATION.setting_key};
    return keys;
}
QJsonArray default_layout(const LayoutProfile &profile) {
    QJsonArray result;
    for (const auto &key : profile.order) result.append(QJsonArray{key, profile.default_mode(key)});
    return result;
}
QJsonArray normalize_layout(const QJsonValue &entries, const LayoutProfile &profile) {
    const auto &order = profile.order;
    QJsonArray result;
    QSet<QString> seen;
    for (const auto &value : entries.toArray()) {
        QJsonArray entry;
        if (value.isArray()) entry = value.toArray();
        // Python sequence unpacking also accepts exactly two-character strings.
        else if (value.isString() && value.toString().size() == 2) {
            const auto s = value.toString();
            entry = {s.left(1), s.right(1)};
        }
        if (entry.size() != 2 || !entry[0].isString()) continue;
        const auto key = entry[0].toString();
        if (!order.contains(key) || seen.contains(key)) continue;
        seen.insert(key);
        auto mode = entry[1].toString();
        if (profile.locked.contains(key)) mode = SHOW;
        else if (mode != SHOW && mode != MORE && mode != HIDE) mode = profile.default_mode(key);
        result.append(QJsonArray{key, mode});
    }
    for (int index = 0; index < order.size(); ++index) {
        const auto key = order[index];
        if (seen.contains(key)) continue;
        int position = 0;
        if (index) {
            for (int i = 0; i < result.size(); ++i) {
                if (result[i].toArray()[0].toString() == order[index - 1]) { position = i + 1; break; }
            }
        }
        result.insert(position, QJsonArray{key, profile.default_mode(key)});
        seen.insert(key);
    }
    return result;
}
QJsonArray load_layout(const ToolSettingsManager &config, const LayoutProfile &profile) {
    const auto data = config.get_app_setting(profile.setting_key).toString().toUtf8();
    return normalize_layout_json(data, profile);
}
QJsonArray normalize_layout_json(const QByteArray &json, const LayoutProfile &profile) {
    const auto doc = QJsonDocument::fromJson(json);
    if (!doc.isArray()) return default_layout(profile);
    auto entries = doc.array();
    const auto slices = members(json);
    for (int index = 0; index < entries.size() && index < slices.size(); ++index) {
        if (!entries[index].isObject()) continue;
        QJsonArray keys;
        QSet<QString> seen;
        for (const auto &member : members(slices[index])) {
            const auto key = object_key(member);
            if (!seen.contains(key)) { keys.append(key); seen.insert(key); }
        }
        entries[index] = keys;
    }
    return normalize_layout(entries, profile);
}
QJsonArray save_layout(ToolSettingsManager &config, const QJsonValue &entries, const LayoutProfile &profile) {
    const auto result = normalize_layout(entries, profile);
    auto serialized = QJsonDocument(result).toJson(QJsonDocument::Compact);
    // Only fixed ASCII identifiers occur here; Python json.dumps adds spaces.
    serialized.replace(",", ", ");
    config.set_app_setting(profile.setting_key, QString::fromUtf8(serialized));
    return result;
}
}
