#include "core/global_hotkey.h"
#include <QHash>
#include <QSet>
#include <stdexcept>
#include <vector>

namespace jietuba::global_hotkey {
namespace {
// The order they are written in, matching the recorder and the input hub.
const std::pair<const char *, int> MODIFIERS[] = {{"ctrl", MOD_CONTROL}, {"shift", MOD_SHIFT}, {"alt", MOD_ALT}, {"win", MOD_WIN}};

const QHash<QString, int> &modifier_aliases() {
    static const QHash<QString, int> aliases = {
        {"ctrl", MOD_CONTROL}, {"control", MOD_CONTROL}, {"shift", MOD_SHIFT}, {"alt", MOD_ALT},
        {"win", MOD_WIN}, {"meta", MOD_WIN}, {"super", MOD_WIN},
    };
    return aliases;
}

struct NamedKey {
    int vk;
    std::vector<const char *> names;  // the first is what the recorder writes
};

// Keys without a letter, digit or F-number name.
const std::vector<NamedKey> &named_keys() {
    static const std::vector<NamedKey> keys = {
        {0x08, {"backspace"}},
        {0x09, {"tab", "backtab"}},
        {0x0C, {"clear"}},
        {0x0D, {"enter", "return"}},
        {0x13, {"pause"}},
        {0x1B, {"esc", "escape"}},
        {0x20, {"space"}},
        {0x21, {"pageup"}},
        {0x22, {"pagedown"}},
        {0x23, {"end"}},
        {0x24, {"home"}},
        {0x25, {"left"}},
        {0x26, {"up"}},
        {0x27, {"right"}},
        {0x28, {"down"}},
        {VK_SNAPSHOT, {"print", "printscreen", "prtsc", "sysreq"}},
        {0x2D, {"insert"}},
        {VK_DELETE, {"delete", "del"}},
        {0xBA, {";", "semicolon"}},
        {0xBB, {"=", "equals", "equal"}},
        {0xBC, {",", "comma"}},
        {0xBD, {"-", "minus"}},
        {0xBE, {".", "period"}},
        {0xBF, {"/", "slash"}},
        {0xC0, {"`", "oem3", "backquote", "grave"}},
        {0xDB, {"[", "lbracket"}},
        {0xDC, {"\\", "backslash"}},
        {0xDD, {"]", "rbracket"}},
        {0xDE, {"'", "quote"}},
    };
    return keys;
}

struct KeyTables {
    QHash<QString, int> name_to_vk;
    QHash<int, QString> vk_to_name;
};

const KeyTables &key_tables() {
    static const KeyTables tables = [] {
        KeyTables result;
        for (const auto &key : named_keys()) {
            result.vk_to_name[key.vk] = QString::fromLatin1(key.names.front());
            for (const char *name : key.names) result.name_to_vk[QString::fromLatin1(name)] = key.vk;
        }
        for (int vk = 'A'; vk <= 'Z'; ++vk) {
            const QString name = QString(QChar(vk)).toLower();
            result.vk_to_name[vk] = name;
            result.name_to_vk[name] = vk;
        }
        for (int vk = '0'; vk <= '9'; ++vk) {
            result.vk_to_name[vk] = QString(QChar(vk));
            result.name_to_vk[QString(QChar(vk))] = vk;
        }
        for (int n = 1; n <= 24; ++n) {
            result.vk_to_name[0x6F + n] = QString("f%1").arg(n);
            result.name_to_vk[QString("f%1").arg(n)] = 0x6F + n;
        }
        return result;
    }();
    return tables;
}

// Keys that bind without modifiers: function keys and keys that type nothing.
bool binds_alone(int vk) {
    static const QSet<int> keys = {0x13, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, VK_SNAPSHOT, 0x2D};
    return keys.contains(vk) || (0x70 <= vk && vk < 0x88);
}
}

QString route_name(Route route) {
    switch (route) {
    case Route::Unset: return "unset";
    case Route::Invalid: return "invalid";
    case Route::Reserved: return "reserved";
    case Route::Mouse: return "mouse";
    case Route::Register: return "register";
    case Route::Hook: return "hook";
    }
    return {};
}

bool is_mouse_button(const QString &text) { return text == MOUSE_BUTTON_BACK || text == MOUSE_BUTTON_FORWARD; }

std::pair<int, int> parse_hotkey(const QString &text) {
    int mods = 0;
    QStringList keys;
    for (const auto &raw : text.split('+')) {
        const QString part = raw.trimmed().toLower();
        // "ctrl++" and the like are unfinished prefixes.
        if (part.isEmpty()) throw std::invalid_argument(("incomplete hotkey: " + text).toStdString());
        if (const auto it = modifier_aliases().constFind(part); it != modifier_aliases().constEnd()) mods |= *it;
        else keys.append(part);
    }
    if (keys.size() != 1) throw std::invalid_argument(("expected exactly one main key: " + text).toStdString());
    const auto it = key_tables().name_to_vk.constFind(keys.front());
    if (it == key_tables().name_to_vk.constEnd()) throw std::invalid_argument(("unsupported key: " + keys.front()).toStdString());
    return {mods, *it};
}

std::optional<QString> format_hotkey(int mods, int vk) {
    const auto it = key_tables().vk_to_name.constFind(vk);
    if (it == key_tables().vk_to_name.constEnd()) return std::nullopt;
    QStringList parts = modifier_names(mods);
    parts.append(*it);
    return parts.join('+');
}

int modifier_mask(const QStringList &names) {
    int mask = 0;
    for (const auto &name : names) mask |= modifier_aliases().value(name, 0);
    return mask;
}

QStringList modifier_names(int mods) {
    QStringList names;
    for (const auto &[name, bit] : MODIFIERS)
        if (mods & bit) names.append(QString::fromLatin1(name));
    return names;
}

Route route(const QString &text) {
    const QString normalized = text.trimmed().toLower();
    if (normalized.isEmpty()) return Route::Unset;
    if (is_mouse_button(normalized)) return Route::Mouse;
    try {
        const auto [mods, vk] = parse_hotkey(normalized);
        return route_of(mods, vk);
    } catch (const std::invalid_argument &) {
        return Route::Invalid;
    }
}

Route route_of(int mods, int vk) {
    mods &= ~MOD_NOREPEAT;
    // Win+L locks and Ctrl+Alt+Del is the secure attention sequence, both handled before
    // any hook; extra modifiers change nothing.
    if ((mods & MOD_WIN) && vk == VK_L) return Route::Reserved;
    if ((mods & MOD_CONTROL) && (mods & MOD_ALT) && vk == VK_DELETE) return Route::Reserved;
    if (!mods && !binds_alone(vk)) return Route::Invalid;
    // The combinations the system keeps are mostly Win ones; PrtSc may register and still open the system capture.
    if ((mods & MOD_WIN) || vk == VK_SNAPSHOT) return Route::Hook;
    return Route::Register;
}
}
