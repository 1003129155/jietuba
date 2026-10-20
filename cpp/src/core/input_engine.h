#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

// The input state machine behind InputHub: shared by the hook thread and the tests, it
// calls no system API itself.
//
// One event goes through fixed stages: global gestures → side-button hotkeys, or for the
// keyboard the hotkey recording and the bound keyboard hotkeys → the watchers. An event
// swallowed by an earlier stage skips the later hotkey stages, so a gesture whose modifiers
// match exactly wins over a side-button hotkey, and recording wins over a bound hotkey.
namespace jietuba::input {

constexpr uint32_t VK_ESCAPE_KEY = 0x1B;
// PrintScreen types nothing, so it can be taken or recorded without modifiers.
constexpr uint32_t VK_PRINT_SCREEN = 0x2C;
// An unassigned key, injected to keep a released Win/Alt from opening its menu.
constexpr uint32_t VK_MENU_MASK = 0xE8;

enum class Button : uint8_t { Left, Middle, Right, X1, X2 };
constexpr std::array<Button, 5> ALL_BUTTONS{Button::Left, Button::Middle, Button::Right, Button::X1, Button::X2};
const char *button_name(Button button);
std::optional<Button> parse_button(std::string_view name);

// A set of modifiers; either side of a modifier counts as held.
class Modifiers {
public:
    static const Modifiers CTRL, SHIFT, ALT, WIN;
    constexpr Modifiers() = default;
    bool contains(Modifiers other) const { return other.bits_ != 0 && (bits_ & other.bits_) == other.bits_; }
    int count() const;
    // nullopt when a name is unknown.
    static std::optional<Modifiers> parse(const std::vector<std::string> &names);
    std::vector<std::string> names() const;
    // The modifier a virtual key belongs to; the generic VK_SHIFT/VK_CONTROL/VK_MENU count too.
    static std::optional<Modifiers> of_key(uint32_t vk);
    Modifiers operator|(Modifiers other) const { return Modifiers(bits_ | other.bits_); }
    bool operator==(Modifiers other) const { return bits_ == other.bits_; }
    bool operator!=(Modifiers other) const { return bits_ != other.bits_; }
    bool operator<(Modifiers other) const { return bits_ < other.bits_; }
    uint8_t bits() const { return bits_; }

private:
    constexpr explicit Modifiers(int bits) : bits_(static_cast<uint8_t>(bits)) {}
    uint8_t bits_ = 0;
};

struct Rect {
    int32_t left = 0, top = 0, right = 0, bottom = 0;
    bool contains(int32_t x, int32_t y) const { return left <= x && x < right && top <= y && y < bottom; }
};

struct MouseInput {
    enum class Action { Move, Down, Up, Wheel };
    Action action = Action::Move;
    Button button = Button::Left;  // Down and Up
    // Wheel: a multiple of WHEEL_DELTA (120); vertical positive is up, horizontal positive is right.
    int32_t delta = 0;
    bool horizontal = false;
    int32_t x = 0, y = 0;
    // Simulated input; the caller passes simulated input carrying the test marker as real.
    bool injected = false;

    static MouseInput move(int32_t x, int32_t y) { return {Action::Move, Button::Left, 0, false, x, y, false}; }
    static MouseInput down(Button b, int32_t x, int32_t y) { return {Action::Down, b, 0, false, x, y, false}; }
    static MouseInput up(Button b, int32_t x, int32_t y) { return {Action::Up, b, 0, false, x, y, false}; }
    static MouseInput wheel(int32_t delta, bool horizontal, int32_t x, int32_t y) {
        return {Action::Wheel, Button::Left, delta, horizontal, x, y, false};
    }
};

struct KeyInput {
    uint32_t vk = 0;
    bool pressed = false;
    bool injected = false;
};

struct Event {
    enum class Kind {
        GestureStart,
        GestureFinish,
        GestureCancel,
        GestureMoved,  // the drag moved; sent once until take_position collects it
        SideButton,
        Hotkey,    // a keyboard hotkey pressed; not again for its auto-repeat
        Captured,  // a combination taken while recording (configure_key_capture); not again for auto-repeat
        Wheel,
        Key,
        Foreground,  // the foreground window changed; every change while anyone watches
        MouseButton,  // a button pressed or released that no gesture or side-button hotkey took
        PointerMoved,  // the pointer moved; sent once per watcher until take_pointer collects the position
        Failure,
    };
    Kind kind = Kind::Failure;
    uint64_t id = 0;  // gestures
    int32_t x = 0, y = 0;  // gestures, wheel and mouse buttons
    Button button = Button::Left;  // SideButton and MouseButton
    std::string text;  // hotkey name, watcher name or failure message
    Modifiers modifiers;  // Captured
    uint32_t vk = 0;  // Captured and Key
    int32_t delta = 0;  // Wheel
    bool horizontal = false;  // Wheel
    bool pressed = false;  // Key and MouseButton
    intptr_t hwnd = 0;  // Foreground

    static Event gesture(Kind kind, uint64_t id, int32_t x, int32_t y);
    static Event moved(uint64_t id);
    static Event side_button(Button button);
    static Event hotkey(std::string name);
    static Event captured(Modifiers modifiers, uint32_t vk);
    static Event wheel(std::string watcher, int32_t x, int32_t y, int32_t delta, bool horizontal);
    static Event key(std::string watcher, uint32_t vk, bool pressed);
    static Event foreground(intptr_t hwnd);
    static Event mouse_button(std::string watcher, Button button, bool pressed, int32_t x, int32_t y);
    static Event pointer_moved(std::string watcher);
    static Event failure(std::string message);
    bool operator==(const Event &other) const;
    bool operator!=(const Event &other) const { return !(*this == other); }
};

using Events = std::vector<Event>;

// The two system services the state machine needs: Win32 in production, a fake in tests.
class Platform {
public:
    virtual ~Platform() = default;
    // Whether this virtual key is down right now (GetAsyncKeyState).
    virtual bool key_held(uint32_t vk) const = 0;
    // Sends a meaningless key pair so the Win/Alt released next opens no Start menu or menu
    // bar; returns the error message on failure.
    virtual std::optional<std::string> mask_menu() = 0;
};

class Engine {
public:
    // Input. Returns whether the event is swallowed.
    bool on_mouse(const MouseInput &input, Platform &platform, Events &out);
    bool on_key(const KeyInput &input, Platform &platform, Events &out);

    // Gestures. bindings are (modifiers, button) with 1–2 modifiers, matched exactly against
    // the held ones. Returns whether the configuration is valid; an invalid one never enables.
    bool configure_gestures(std::vector<std::pair<Modifiers, Button>> bindings, bool enabled, Events &out);
    // The UI decides when gestures are unavailable; the hook looks at no windows or modal state.
    void set_blocked(bool blocked, Events &out);
    // Invalidates the current and earlier gestures, including start and finish events queued
    // but not yet handled by the UI.
    void cancel(Events &out);
    // Collects the latest position; notifications of newer gestures are unaffected.
    std::optional<std::pair<int32_t, int32_t>> take_position(uint64_t gesture_id);
    bool dragging() const { return dragging_; }
    // Whether queued start and finish events still count; reconfiguring and cancelling void them.
    bool accepts(uint64_t gesture_id) const;
    // The current gesture only; a new gesture may start before the old one's start event arrives.
    std::optional<std::pair<Modifiers, Button>> gesture_binding(uint64_t gesture_id) const;
    uint64_t gesture_id() const { return gesture_id_; }

    // Side buttons in suppressed have press and release swallowed; capture_all swallows every
    // side button (for recording a shortcut).
    void configure_side_buttons(bool enabled, const std::vector<Button> &suppressed, bool capture_all);
    // vk pressed with exactly these modifiers has its press, auto-repeat and release swallowed
    // and reports once. A binding of the same name is replaced. Refused (false) without a
    // modifier (except PrtSc: taking it alone swallows no typing) or when vk is a modifier.
    bool bind_hotkey(const std::string &name, Modifiers modifiers, uint32_t vk);
    // A key whose press was already swallowed still has its release swallowed.
    void unbind_hotkey(const std::string &name);
    // While recording a shortcut: keys pressed with Win held, and PrtSc, are swallowed as a pair
    // and reported as Captured, ahead of bound hotkeys; a lone Win opens no Start menu. A press
    // swallowed before turning it off still has its release swallowed.
    void configure_key_capture(bool enabled);
    void watch_wheel(const std::string &watcher, std::optional<Rect> rect);
    void unwatch_wheel(const std::string &watcher);
    void watch_keys(const std::string &watcher, std::vector<uint32_t> keys);
    void unwatch_keys(const std::string &watcher);
    // Every button press and release the earlier stages leave alone, with where it happened, and one
    // notice for moves until take_pointer collects the latest position. Nothing is swallowed.
    void watch_mouse(const std::string &watcher);
    void unwatch_mouse(const std::string &watcher);
    std::optional<std::pair<int32_t, int32_t>> take_pointer(const std::string &watcher);
    void watch_foreground(const std::string &watcher);
    void unwatch_foreground(const std::string &watcher);
    // One event per change, however many watch.
    void on_foreground(intptr_t hwnd, Events &out);

    // Whether the low-level hooks are needed: a feature is on, or a swallowed press still
    // waits for its release.
    bool hooks_needed() const;
    bool foreground_needed() const;
    // Takes the request to re-evaluate which hooks are needed.
    bool take_sync_request();
    // The hooks failed: turns everything off and drops unpaired claims, swallowing nothing more.
    void fail(const std::string &message, Events &out);
    void close(Events &out);

private:
    // Keys in insertion order; few enough that a vector beats a set.
    class KeySet {
    public:
        void insert(uint32_t vk);
        bool remove(uint32_t vk);
        bool contains(uint32_t vk) const;
        bool empty() const { return keys_.empty(); }
        void clear() { keys_.clear(); }
    private:
        std::vector<uint32_t> keys_;
    };

    bool gesture_mouse(const MouseInput &input, Platform &platform, Events &out);
    bool button_down(Button button, Platform &platform, Events &out);
    bool button_up(Button button, Events &out);
    void mask_menu_on_release(Modifiers held, const Platform &platform);
    bool hotkey_key(const KeyInput &input, Platform &platform, Events &out);
    bool capture_key(const KeyInput &input, Platform &platform, Events &out);
    bool gesture_key(const KeyInput &input, Platform &platform, Events &out);
    bool side_mouse(const MouseInput &input, Events &out);
    void observe_mouse(const MouseInput &input, Events &out);
    void clear_claims();

    // Global gestures
    std::vector<std::pair<Modifiers, Button>> bindings_;
    bool enabled_ = false;
    bool blocked_ = false;
    bool closed_ = false;
    uint64_t gesture_id_ = 0;
    uint64_t invalid_through_ = 0;
    bool dragging_ = false;
    Modifiers gesture_modifiers_;
    std::optional<Button> gesture_button_;
    int32_t x_ = 0, y_ = 0;
    std::optional<uint64_t> pending_move_;
    // Buttons whose press was swallowed; their release must be swallowed too.
    uint8_t claimed_ = 0;
    bool claimed_escape_ = false;
    // Win/Alt held during a gesture or hotkey; their release first masks the menu.
    KeySet pending_menu_keys_;
    // Side-button hotkeys
    bool side_enabled_ = false;
    uint8_t side_suppressed_ = 0;
    bool side_capture_all_ = false;
    uint8_t side_claimed_ = 0;
    // Keyboard hotkeys
    std::vector<std::tuple<std::string, Modifiers, uint32_t>> hotkeys_;
    // Keys whose press a hotkey swallowed; their auto-repeat and release must be swallowed too.
    KeySet hotkey_claimed_;
    // Shortcut recording
    bool key_capture_ = false;
    // Keys whose press recording swallowed; their auto-repeat and release must be swallowed too.
    KeySet capture_claimed_;
    // Watchers
    std::vector<std::pair<std::string, std::optional<Rect>>> wheel_watchers_;
    std::vector<std::pair<std::string, std::vector<uint32_t>>> key_watchers_;
    // Mouse watchers with whether a move notice waits to be collected.
    std::vector<std::pair<std::string, bool>> mouse_watchers_;
    std::vector<std::string> foreground_watchers_;
    bool sync_requested_ = false;
};
}
