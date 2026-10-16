#pragma once
#include <QJsonValue>
#include <QList>
#include <QString>
#include <map>
#include <utility>

// The Special Paste menu's built-in options, their order and on/off state.
namespace jietuba {
class ToolSettingsManager;

namespace special_paste {
inline const QString SETTING_KEY = "clipboard_special_paste";

struct PasteOption {
    QString key;
    const char *label;
    const char *description;
    QString content_type = "text";
    QString group = "text";
    bool enabled = true;
};

using Entry = std::pair<QString, bool>;

const QList<PasteOption> &BUILTIN_OPTIONS();
const std::map<QString, PasteOption> &OPTIONS_BY_KEY();
QList<Entry> default_entries();
// Keeps the stored order and switches, drops unknown or repeated keys, appends new built-ins; all may be off.
QList<Entry> normalize(const QJsonValue &entries);
QList<Entry> normalize(const QList<Entry> &entries);
QList<Entry> load(const ToolSettingsManager *config_manager);
void save(ToolSettingsManager *config_manager, const QList<Entry> &entries);
// json.dumps of the entry list, as stored in the setting.
QString dumps(const QList<Entry> &entries);
}
}
