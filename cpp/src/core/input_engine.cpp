#include "core/input_engine.h"
#include <algorithm>
#include <utility>

namespace jietuba::input {
namespace {
constexpr uint32_t VK_LWIN_KEY = 0x5B;
constexpr uint32_t VK_RWIN_KEY = 0x5C;
constexpr uint32_t VK_LALT_KEY = 0xA4;
constexpr uint32_t VK_RALT_KEY = 0xA5;

uint8_t bit(Button button) { return static_cast<uint8_t>(1u << static_cast<uint8_t>(button)); }

struct NamedModifier {
    Modifiers modifier;
    const char *name;
    std::array<uint32_t, 2> keys;
};

const std::array<NamedModifier, 4> &named_modifiers() {
    static const std::array<NamedModifier, 4> named{{
        {Modifiers::CTRL, "ctrl", {0xA2, 0xA3}},
        {Modifiers::SHIFT, "shift", {0xA0, 0xA1}},
        {Modifiers::ALT, "alt", {VK_LALT_KEY, VK_RALT_KEY}},
        {Modifiers::WIN, "win", {VK_LWIN_KEY, VK_RWIN_KEY}},
    }};
    return named;
}

// exclude_vk: the key being released, whose asynchronous state is not yet updated.
Modifiers held_modifiers(const Platform &platform, std::optional<uint32_t> exclude_vk) {
    Modifiers held;
    for (const auto &named : named_modifiers()) {
        for (uint32_t vk : named.keys) {
            if (exclude_vk != vk && platform.key_held(vk)) {
                held = held | named.modifier;
                break;
            }
        }
    }
    return held;
}
}

const Modifiers Modifiers::CTRL(1);
const Modifiers Modifiers::SHIFT(2);
const Modifiers Modifiers::ALT(4);
const Modifiers Modifiers::WIN(8);

const char *button_name(Button button) {
    switch (button) {
    case Button::Left: return "left";
    case Button::Middle: return "middle";
    case Button::Right: return "right";
    case Button::X1: return "x1";
    case Button::X2: return "x2";
    }
    return "";
}

std::optional<Button> parse_button(std::string_view name) {
    for (Button button : ALL_BUTTONS) {
        if (name == button_name(button)) return button;
    }
    return std::nullopt;
}

int Modifiers::count() const {
    int count = 0;
    for (uint8_t bits = bits_; bits; bits &= static_cast<uint8_t>(bits - 1)) ++count;
    return count;
}

std::optional<Modifiers> Modifiers::parse(const std::vector<std::string> &names) {
    Modifiers result;
    for (const auto &name : names) {
        const auto &table = named_modifiers();
        auto it = std::find_if(table.begin(), table.end(), [&](const NamedModifier &n) { return name == n.name; });
        if (it == table.end()) return std::nullopt;
        result = result | it->modifier;
    }
    return result;
}

std::vector<std::string> Modifiers::names() const {
    std::vector<std::string> names;
    for (const auto &named : named_modifiers()) {
        if (contains(named.modifier)) names.emplace_back(named.name);
    }
    return names;
}

std::optional<Modifiers> Modifiers::of_key(uint32_t vk) {
    switch (vk) {
    case 0x10: return SHIFT;
    case 0x11: return CTRL;
    case 0x12: return ALT;
    default: break;
    }
    for (const auto &named : named_modifiers()) {
        if (named.keys[0] == vk || named.keys[1] == vk) return named.modifier;
    }
    return std::nullopt;
}

Event Event::gesture(Kind kind, uint64_t id, int32_t x, int32_t y) {
    Event e;
    e.kind = kind;
    e.id = id;
    e.x = x;
    e.y = y;
    return e;
}

Event Event::moved(uint64_t id) {
    Event e;
    e.kind = Kind::GestureMoved;
    e.id = id;
    return e;
}

Event Event::side_button(Button button) {
    Event e;
    e.kind = Kind::SideButton;
    e.button = button;
    return e;
}

Event Event::hotkey(std::string name) {
    Event e;
    e.kind = Kind::Hotkey;
    e.text = std::move(name);
    return e;
}

Event Event::captured(Modifiers modifiers, uint32_t vk) {
    Event e;
    e.kind = Kind::Captured;
    e.modifiers = modifiers;
    e.vk = vk;
    return e;
}

Event Event::wheel(std::string watcher, int32_t x, int32_t y, int32_t delta, bool horizontal) {
    Event e;
    e.kind = Kind::Wheel;
    e.text = std::move(watcher);
    e.x = x;
    e.y = y;
    e.delta = delta;
    e.horizontal = horizontal;
    return e;
}

Event Event::key(std::string watcher, uint32_t vk, bool pressed) {
    Event e;
    e.kind = Kind::Key;
    e.text = std::move(watcher);
    e.vk = vk;
    e.pressed = pressed;
    return e;
}

Event Event::foreground(intptr_t hwnd) {
    Event e;
    e.kind = Kind::Foreground;
    e.hwnd = hwnd;
    return e;
}

Event Event::failure(std::string message) {
    Event e;
    e.kind = Kind::Failure;
    e.text = std::move(message);
    return e;
}

bool Event::operator==(const Event &o) const {
    return kind == o.kind && id == o.id && x == o.x && y == o.y && button == o.button && text == o.text
        && modifiers == o.modifiers && vk == o.vk && delta == o.delta && horizontal == o.horizontal
        && pressed == o.pressed && hwnd == o.hwnd;
}

void Engine::KeySet::insert(uint32_t vk) {
    if (!contains(vk)) keys_.push_back(vk);
}

bool Engine::KeySet::remove(uint32_t vk) {
    auto it = std::remove(keys_.begin(), keys_.end(), vk);
    const bool removed = it != keys_.end();
    keys_.erase(it, keys_.end());
    return removed;
}

bool Engine::KeySet::contains(uint32_t vk) const { return std::find(keys_.begin(), keys_.end(), vk) != keys_.end(); }

// ---------------------------------------------------------------------- input

bool Engine::on_mouse(const MouseInput &input, Platform &platform, Events &out) {
    if (closed_) return false;
    // Remote-control software delivers mouse input as injected; gestures depend on the
    // configured modifiers and button, not on where the input came from.
    if (gesture_mouse(input, platform, out)) return true;
    if (side_mouse(input, out)) return true;
    observe_mouse(input, out);
    return false;
}

bool Engine::on_key(const KeyInput &input, Platform &platform, Events &out) {
    if (closed_) return false;
    // An injected Esc or modifier release must cancel and drain the same gesture. Recording
    // comes before the bound hotkeys, so a combination already bound is recorded instead of
    // triggering.
    const bool suppress = gesture_key(input, platform, out) || capture_key(input, platform, out)
        || hotkey_key(input, platform, out);
    for (const auto &[watcher, keys] : key_watchers_) {
        if (std::find(keys.begin(), keys.end(), input.vk) != keys.end())
            out.push_back(Event::key(watcher, input.vk, input.pressed));
    }
    return suppress;
}

bool Engine::gesture_mouse(const MouseInput &input, Platform &platform, Events &out) {
    x_ = input.x;
    y_ = input.y;
    switch (input.action) {
    case MouseInput::Action::Down: return button_down(input.button, platform, out);
    case MouseInput::Action::Up: return button_up(input.button, out);
    case MouseInput::Action::Move:
        if (dragging_ && !pending_move_) {
            // High-rate moves only replace the latest position; no repeat until the UI collects it.
            pending_move_ = gesture_id_;
            out.push_back(Event::moved(gesture_id_));
        }
        // The cursor moves as usual; the paired button events are swallowed, so nothing
        // underneath can start a drag.
        return false;
    case MouseInput::Action::Wheel:
        // The desktop keeps repainting during a selection, and scrolling would move what is being selected.
        return dragging_;
    }
    return false;
}

bool Engine::button_down(Button button, Platform &platform, Events &out) {
    if (claimed_ & bit(button)) return true;
    if (dragging_) {
        // Like the wheel: a click would change what is being selected.
        claimed_ |= bit(button);
        return true;
    }
    if (!enabled_ || blocked_) return false;
    const Modifiers held = held_modifiers(platform, std::nullopt);
    if (std::find(bindings_.begin(), bindings_.end(), std::make_pair(held, button)) == bindings_.end()) return false;
    gesture_modifiers_ = held;
    gesture_button_ = button;
    ++gesture_id_;
    pending_move_.reset();
    dragging_ = true;
    claimed_ |= bit(button);
    mask_menu_on_release(held, platform);
    out.push_back(Event::gesture(Event::Kind::GestureStart, gesture_id_, x_, y_));
    return true;
}

bool Engine::button_up(Button button, Events &out) {
    if (!(claimed_ & bit(button))) return false;
    claimed_ &= static_cast<uint8_t>(~bit(button));
    if (gesture_button_ == button) {
        pending_move_.reset();
        if (dragging_) {
            dragging_ = false;
            out.push_back(Event::gesture(Event::Kind::GestureFinish, gesture_id_, x_, y_));
        }
    }
    sync_requested_ = true;
    return true;
}

// Remembers the Win/Alt held now so their release masks the menu first. Win's auto-repeat
// can reactivate the Start menu after an early mask, so they stay remembered until released,
// even after the gesture or hotkey ends, is cancelled or the settings change.
void Engine::mask_menu_on_release(Modifiers held, const Platform &platform) {
    const std::pair<Modifiers, std::array<uint32_t, 2>> menu_keys[] = {
        {Modifiers::WIN, {VK_LWIN_KEY, VK_RWIN_KEY}},
        {Modifiers::ALT, {VK_LALT_KEY, VK_RALT_KEY}},
    };
    for (const auto &[modifier, keys] : menu_keys) {
        if (!held.contains(modifier)) continue;
        for (uint32_t vk : keys) {
            if (platform.key_held(vk)) pending_menu_keys_.insert(vk);
        }
    }
}

bool Engine::hotkey_key(const KeyInput &input, Platform &platform, Events &out) {
    // Remote-control tools inject keyboard input too; bindings match regardless of origin,
    // as mouse gestures do.
    if (!input.pressed) {
        if (hotkey_claimed_.remove(input.vk)) {
            sync_requested_ = true;
            return true;
        }
        return false;
    }
    if (hotkey_claimed_.contains(input.vk)) return true;
    // The hook runs before the key's asynchronous state updates, so a key already down is
    // auto-repeating from before the modifiers; its press went to another program, and
    // swallowing its release would leave that program thinking it is held.
    if (platform.key_held(input.vk)) return false;
    const Modifiers held = held_modifiers(platform, std::nullopt);
    auto it = std::find_if(hotkeys_.begin(), hotkeys_.end(), [&](const auto &hotkey) {
        return std::get<2>(hotkey) == input.vk && std::get<1>(hotkey) == held;
    });
    if (it == hotkeys_.end()) return false;
    std::string name = std::get<0>(*it);
    hotkey_claimed_.insert(input.vk);
    mask_menu_on_release(held, platform);
    out.push_back(Event::hotkey(std::move(name)));
    return true;
}

// While recording, takes the combinations the system would handle itself: keys pressed with
// Win held, and PrtSc. Other keys reach the recording edit as usual.
bool Engine::capture_key(const KeyInput &input, Platform &platform, Events &out) {
    // The menu mask also arrives with Win held; swallowing it would let the Start menu open.
    if (input.vk == VK_MENU_MASK) return false;
    if (!input.pressed) {
        if (capture_claimed_.remove(input.vk)) {
            sync_requested_ = true;
            return true;
        }
        return false;
    }
    if (capture_claimed_.contains(input.vk)) return true;
    if (!key_capture_) return false;
    if (auto modifier = Modifiers::of_key(input.vk)) {
        // A lone Win must not open the Start menu either; it would take focus from the edit.
        if (*modifier == Modifiers::WIN) pending_menu_keys_.insert(input.vk);
        return false;
    }
    // As in hotkey_key: a key down before the modifiers is auto-repeating for another program.
    if (platform.key_held(input.vk)) return false;
    const Modifiers held = held_modifiers(platform, std::nullopt);
    if (!held.contains(Modifiers::WIN) && input.vk != VK_PRINT_SCREEN) return false;
    capture_claimed_.insert(input.vk);
    mask_menu_on_release(held, platform);
    out.push_back(Event::captured(held, input.vk));
    return true;
}

bool Engine::gesture_key(const KeyInput &input, Platform &platform, Events &out) {
    const std::optional<Modifiers> modifier = Modifiers::of_key(input.vk);
    const bool menu_modifier = modifier && (*modifier == Modifiers::WIN || *modifier == Modifiers::ALT);
    if (input.pressed && dragging_ && menu_modifier && gesture_modifiers_.contains(*modifier))
        pending_menu_keys_.insert(input.vk);
    if (!input.pressed && pending_menu_keys_.remove(input.vk)) {
        // Masked before this release is passed on; the real release must not be swallowed,
        // the system needs it to clear the modifier state.
        if (auto error = platform.mask_menu()) out.push_back(Event::failure(std::move(*error)));
        sync_requested_ = true;
    }
    if (input.vk == VK_ESCAPE_KEY) {
        if (input.pressed && (dragging_ || claimed_escape_)) {
            claimed_escape_ = true;
            cancel(out);
            return true;
        }
        if (!input.pressed && claimed_escape_) {
            claimed_escape_ = false;
            sync_requested_ = true;
            return true;
        }
    }
    if (dragging_ && !input.pressed && modifier) {
        // The hook runs before this key's asynchronous state updates, so leave it out and look
        // at the other side.
        if (gesture_modifiers_.contains(*modifier) && !held_modifiers(platform, input.vk).contains(*modifier))
            cancel(out);
    }
    return false;
}

bool Engine::side_mouse(const MouseInput &input, Events &out) {
    if (input.button != Button::X1 && input.button != Button::X2) return false;
    const uint8_t b = bit(input.button);
    if (input.action == MouseInput::Action::Down) {
        if (side_claimed_ & b) return true;
        if (!side_enabled_) return false;
        out.push_back(Event::side_button(input.button));
        if (side_capture_all_ || (side_suppressed_ & b)) {
            // Press and release are swallowed as a pair; nobody gets a release without its press.
            side_claimed_ |= b;
            return true;
        }
        return false;
    }
    if (input.action == MouseInput::Action::Up && (side_claimed_ & b)) {
        side_claimed_ &= static_cast<uint8_t>(~b);
        sync_requested_ = true;
        return true;
    }
    return false;
}

void Engine::observe_mouse(const MouseInput &input, Events &out) const {
    if (input.action != MouseInput::Action::Wheel) return;
    for (const auto &[watcher, rect] : wheel_watchers_) {
        if (!rect || rect->contains(input.x, input.y))
            out.push_back(Event::wheel(watcher, input.x, input.y, input.delta, input.horizontal));
    }
}

// ---------------------------------------------------------------------- gestures

bool Engine::configure_gestures(std::vector<std::pair<Modifiers, Button>> bindings, bool enabled, Events &out) {
    std::sort(bindings.begin(), bindings.end());
    bindings.erase(std::unique(bindings.begin(), bindings.end()), bindings.end());
    const bool valid = !bindings.empty() && std::all_of(bindings.begin(), bindings.end(), [](const auto &binding) {
        return binding.first.count() >= 1 && binding.first.count() <= 2;
    });
    enabled = enabled && valid && !closed_;
    if (bindings != bindings_ || !enabled) cancel(out);
    bindings_ = std::move(bindings);
    enabled_ = enabled;
    sync_requested_ = true;
    return valid;
}

void Engine::set_blocked(bool blocked, Events &out) {
    blocked_ = blocked;
    if (blocked) cancel(out);
}

void Engine::cancel(Events &out) {
    invalid_through_ = gesture_id_;
    pending_move_.reset();
    if (dragging_) {
        dragging_ = false;
        out.push_back(Event::gesture(Event::Kind::GestureCancel, gesture_id_, x_, y_));
    }
}

std::optional<std::pair<int32_t, int32_t>> Engine::take_position(uint64_t gesture_id) {
    if (pending_move_ != gesture_id) return std::nullopt;
    pending_move_.reset();
    return std::make_pair(x_, y_);
}

bool Engine::accepts(uint64_t gesture_id) const {
    return !closed_ && invalid_through_ < gesture_id && gesture_id <= gesture_id_;
}

std::optional<std::pair<Modifiers, Button>> Engine::gesture_binding(uint64_t gesture_id) const {
    if (gesture_id != gesture_id_ || !gesture_button_) return std::nullopt;
    return std::make_pair(gesture_modifiers_, *gesture_button_);
}

// ---------------------------------------------------------------------- side buttons and watchers

void Engine::configure_side_buttons(bool enabled, const std::vector<Button> &suppressed, bool capture_all) {
    side_enabled_ = enabled;
    side_suppressed_ = 0;
    for (Button button : suppressed) side_suppressed_ |= bit(button);
    side_capture_all_ = capture_all;
    sync_requested_ = true;
}

bool Engine::bind_hotkey(const std::string &name, Modifiers modifiers, uint32_t vk) {
    unbind_hotkey(name);
    const bool bare_allowed = vk == VK_PRINT_SCREEN;
    if ((modifiers.count() == 0 && !bare_allowed) || Modifiers::of_key(vk) || closed_) return false;
    hotkeys_.emplace_back(name, modifiers, vk);
    return true;
}

void Engine::unbind_hotkey(const std::string &name) {
    hotkeys_.erase(std::remove_if(hotkeys_.begin(), hotkeys_.end(), [&](const auto &h) { return std::get<0>(h) == name; }),
        hotkeys_.end());
    sync_requested_ = true;
}

void Engine::configure_key_capture(bool enabled) {
    key_capture_ = enabled && !closed_;
    sync_requested_ = true;
}

void Engine::watch_wheel(const std::string &watcher, std::optional<Rect> rect) {
    unwatch_wheel(watcher);
    wheel_watchers_.emplace_back(watcher, rect);
    sync_requested_ = true;
}

void Engine::unwatch_wheel(const std::string &watcher) {
    wheel_watchers_.erase(std::remove_if(wheel_watchers_.begin(), wheel_watchers_.end(),
                              [&](const auto &w) { return w.first == watcher; }),
        wheel_watchers_.end());
    sync_requested_ = true;
}

void Engine::watch_keys(const std::string &watcher, std::vector<uint32_t> keys) {
    unwatch_keys(watcher);
    key_watchers_.emplace_back(watcher, std::move(keys));
    sync_requested_ = true;
}

void Engine::unwatch_keys(const std::string &watcher) {
    key_watchers_.erase(std::remove_if(key_watchers_.begin(), key_watchers_.end(),
                            [&](const auto &w) { return w.first == watcher; }),
        key_watchers_.end());
    sync_requested_ = true;
}

void Engine::watch_foreground(const std::string &watcher) {
    if (std::find(foreground_watchers_.begin(), foreground_watchers_.end(), watcher) == foreground_watchers_.end()) {
        foreground_watchers_.push_back(watcher);
        sync_requested_ = true;
    }
}

void Engine::unwatch_foreground(const std::string &watcher) {
    foreground_watchers_.erase(std::remove(foreground_watchers_.begin(), foreground_watchers_.end(), watcher),
        foreground_watchers_.end());
    sync_requested_ = true;
}

void Engine::on_foreground(intptr_t hwnd, Events &out) {
    if (foreground_needed()) out.push_back(Event::foreground(hwnd));
}

// ---------------------------------------------------------------------- lifetime

bool Engine::hooks_needed() const {
    return !closed_
        && (enabled_ || claimed_ != 0 || claimed_escape_ || !pending_menu_keys_.empty() || side_enabled_
            || side_claimed_ != 0 || !hotkeys_.empty() || !hotkey_claimed_.empty() || key_capture_
            || !capture_claimed_.empty() || !wheel_watchers_.empty() || !key_watchers_.empty());
}

bool Engine::foreground_needed() const { return !closed_ && !foreground_watchers_.empty(); }

bool Engine::take_sync_request() { return std::exchange(sync_requested_, false); }

void Engine::fail(const std::string &message, Events &out) {
    enabled_ = false;
    side_enabled_ = false;
    hotkeys_.clear();
    key_capture_ = false;
    cancel(out);
    clear_claims();
    sync_requested_ = true;
    out.push_back(Event::failure(message));
}

void Engine::close(Events &out) {
    enabled_ = false;
    cancel(out);
    closed_ = true;
    side_enabled_ = false;
    hotkeys_.clear();
    key_capture_ = false;
    clear_claims();
    wheel_watchers_.clear();
    key_watchers_.clear();
    foreground_watchers_.clear();
    sync_requested_ = true;
}

void Engine::clear_claims() {
    claimed_ = 0;
    claimed_escape_ = false;
    pending_menu_keys_.clear();
    side_claimed_ = 0;
    hotkey_claimed_.clear();
    capture_claimed_.clear();
}
}
