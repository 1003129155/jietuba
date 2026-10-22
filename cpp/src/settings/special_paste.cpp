#include "settings/special_paste.h"
#include "settings/tool_settings.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QStringList>

namespace jietuba::special_paste {
const QList<PasteOption> &BUILTIN_OPTIONS() {
    static const QList<PasteOption> options{
        {"special_paste_plain_text", "Paste Plain Text", "Paste without text formatting.", "text", "plain"},
        {"transform_uppercase", "All Uppercase", "Convert letters to uppercase.", "text", "case"},
        {"transform_lowercase", "All Lowercase", "Convert letters to lowercase.", "text", "case"},
        {"transform_capitalize_words", "Capitalize Words", "Capitalize the first letter of each word.", "text", "case"},
        {"transform_capitalize_sentences", "Capitalize Sentences", "Capitalize the first letter of each sentence.", "text", "case"},
        {"transform_toggle_case", "Toggle Case", "Swap uppercase and lowercase letters.", "text", "case"},
        {"transform_sql_in", "SQL IN Clause", "Format values as a quoted SQL list.", "text", "sql"},
        {"transform_remove_linebreaks", "Remove Line Breaks", "Replace line breaks with spaces."},
        {"transform_remove_whitespace", "Remove All Whitespace", "Remove spaces, tabs and line breaks."},
        {"transform_extract_digits", "Extract Digits", "Keep only digits 0–9.", "text", "text", false},
        {"transform_extract_letters", "Extract Letters", "Keep only English letters A–Z and a–z.", "text", "text", false},
        {"transform_append_time", "Paste with Current Time", "Append the current date and time."},
        {"special_paste_in_order", "Paste in Order", "Paste without moving the item to the top.", "text", "order"},
        {"file_paste_names", "File Name", "Paste file names without their folder paths.", "file"},
        {"file_paste_links", "File Link", "Paste full file paths.", "file"},
    };
    return options;
}

const std::map<QString, PasteOption> &OPTIONS_BY_KEY() {
    static const std::map<QString, PasteOption> by_key = [] {
        std::map<QString, PasteOption> map;
        for (const auto &option : BUILTIN_OPTIONS()) map.emplace(option.key, option);
        return map;
    }();
    return by_key;
}

QList<Entry> default_entries() {
    QList<Entry> entries;
    for (const auto &option : BUILTIN_OPTIONS()) entries.append({option.key, option.enabled});
    return entries;
}

namespace {
QList<Entry> complete(QList<Entry> result, const QSet<QString> &seen) {
    for (const auto &option : BUILTIN_OPTIONS())
        if (!seen.contains(option.key)) result.append({option.key, option.enabled});
    return result;
}
}

QList<Entry> normalize(const QJsonValue &entries) {
    QList<Entry> result;
    QSet<QString> seen;
    for (const auto &entry : entries.toArray()) {
        const auto pair = entry.toArray();
        if (!entry.isArray() || pair.size() != 2 || !pair[0].isString()) continue;
        const QString key = pair[0].toString();
        if (!OPTIONS_BY_KEY().count(key) || seen.contains(key)) continue;
        seen.insert(key);
        result.append({key, pair[1].isBool() ? pair[1].toBool() : OPTIONS_BY_KEY().at(key).enabled});
    }
    return complete(result, seen);
}

QList<Entry> normalize(const QList<Entry> &entries) {
    QList<Entry> result;
    QSet<QString> seen;
    for (const auto &[key, enabled] : entries) {
        if (!OPTIONS_BY_KEY().count(key) || seen.contains(key)) continue;
        seen.insert(key);
        result.append({key, enabled});
    }
    return complete(result, seen);
}

QList<Entry> load(const ToolSettingsManager *config_manager) {
    const QString raw = config_manager ? config_manager->get_clipboard_special_paste() : QString();
    if (raw.isEmpty()) return normalize(QJsonValue());
    const auto document = QJsonDocument::fromJson(raw.toUtf8());
    return normalize(document.isArray() ? QJsonValue(document.array()) : QJsonValue());
}

QString dumps(const QList<Entry> &entries) {
    QStringList parts;
    for (const auto &[key, enabled] : entries) parts.append(QString("[\"%1\", %2]").arg(key, enabled ? "true" : "false"));
    return "[" + parts.join(", ") + "]";
}

void save(ToolSettingsManager *config_manager, const QList<Entry> &entries) { config_manager->set_clipboard_special_paste(dumps(normalize(entries))); }
}
