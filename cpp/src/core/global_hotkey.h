#pragma once
#include <QString>
#include <QStringList>
#include <optional>
#include <utility>

namespace jietuba {
// Side buttons use their own names, parsed and registered apart from key combinations.
inline const QString MOUSE_BUTTON_BACK = "mouseback";
inline const QString MOUSE_BUTTON_FORWARD = "mouseforward";

// Global hotkey key names and how a value registers. The recorder, the check before
// saving, registration and the old-setting migration all follow these results.
namespace global_hotkey {
constexpr int MOD_ALT = 0x0001;
constexpr int MOD_CONTROL = 0x0002;
constexpr int MOD_SHIFT = 0x0004;
constexpr int MOD_WIN = 0x0008;
constexpr int MOD_NOREPEAT = 0x4000;
constexpr int VK_SNAPSHOT = 0x2C;
constexpr int VK_DELETE = 0x2E;
constexpr int VK_L = 'L';

enum class Route {
    Unset,     // empty
    Invalid,   // unparseable, or a typing key without modifiers
    Reserved,  // handled by the system before any hook
    Mouse,     // side button, through the hook
    Register,  // system registration; the hook takes it over while another app holds it
    Hook,      // the hook takes it over, with the system registration as a backstop
};
// The reference's names: "unset", "invalid", "reserved", "mouse", "register", "hook".
QString route_name(Route route);

bool is_mouse_button(const QString &text);
// "ctrl+shift+a" -> (modifier mask, virtual key) without MOD_NOREPEAT; throws std::invalid_argument.
std::pair<int, int> parse_hotkey(const QString &text);
// (modifier mask, virtual key) -> the recorder's spelling; nullopt for a key not in the table.
std::optional<QString> format_hotkey(int mods, int vk);
// Modifier names the input hub reports -> mask.
int modifier_mask(const QStringList &names);
// Mask -> the modifier names the input hub takes.
QStringList modifier_names(int mods);
Route route(const QString &text);
// route() after parsing.
Route route_of(int mods, int vk);
// Whether the input hub can take the key over: it binds only combinations with a modifier, and
// PrtSc, so a key bound alone that another app holds stays unavailable.
bool hook_can_bind(int mods, int vk);
}
}
