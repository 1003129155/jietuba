#include "core/input_engine.h"
#include <QTest>
#include <algorithm>
#include <set>

using namespace jietuba::input;

namespace {
constexpr uint32_t LCTRL = 0xA2;
constexpr uint32_t RCTRL = 0xA3;
constexpr uint32_t LSHIFT = 0xA0;
constexpr uint32_t LALT = 0xA4;
constexpr uint32_t RALT = 0xA5;
constexpr uint32_t LWIN = 0x5B;
constexpr uint32_t RWIN = 0x5C;
constexpr uint32_t KEY_V = 0x56;
constexpr uint32_t KEY_S = 0x53;
constexpr uint32_t KEY_J = 0x4A;

struct FakePlatform : Platform {
    std::set<uint32_t> held;
    int masks = 0;
    std::optional<std::string> mask_error;
    bool key_held(uint32_t vk) const override { return held.count(vk) != 0; }
    std::optional<std::string> mask_menu() override {
        ++masks;
        return mask_error;
    }
};

using Result = std::pair<bool, Events>;

struct Rig {
    Engine engine;
    FakePlatform platform;

    explicit Rig(const std::vector<std::pair<Modifiers, Button>> &bindings = {}) {
        Events ignored;
        if (!bindings.empty() && !engine.configure_gestures(bindings, true, ignored)) qFatal("invalid bindings");
    }
    void hold(uint32_t vk) { platform.held.insert(vk); }
    void release(uint32_t vk) { platform.held.erase(vk); }
    Result mouse(MouseInput input) {
        Events events;
        const bool suppress = engine.on_mouse(input, platform, events);
        return {suppress, events};
    }
    // Real key order: the hook runs before the key's asynchronous state updates.
    Result key(uint32_t vk, bool pressed) {
        Events events;
        const bool suppress = engine.on_key(KeyInput{vk, pressed, false}, platform, events);
        if (pressed) hold(vk);
        else release(vk);
        return {suppress, events};
    }
    Result raw_key(KeyInput input) {
        Events events;
        const bool suppress = engine.on_key(input, platform, events);
        return {suppress, events};
    }
    template <class F>
    Events with(F f) {
        Events events;
        f(engine, events);
        return events;
    }
    uint64_t start_ctrl_left() {
        hold(LCTRL);
        auto [suppress, events] = mouse(MouseInput::down(Button::Left, 10, 20));
        if (!suppress || events.size() != 1 || events[0].kind != Event::Kind::GestureStart || events[0].x != 10 || events[0].y != 20)
            qFatal("expected a start");
        return events[0].id;
    }
};

Modifiers ctrl() { return Modifiers::CTRL; }
Modifiers ctrl_alt() { return *Modifiers::parse({"ctrl", "alt"}); }
Event start(uint64_t id, int x, int y) { return Event::gesture(Event::Kind::GestureStart, id, x, y); }
Event finish(uint64_t id, int x, int y) { return Event::gesture(Event::Kind::GestureFinish, id, x, y); }
Event cancel(uint64_t id, int x, int y) { return Event::gesture(Event::Kind::GestureCancel, id, x, y); }
Event clipboard_hotkey() { return Event::hotkey("clipboard"); }
MouseInput injected(MouseInput input) {
    input.injected = true;
    return input;
}

Rig side_rig(const std::vector<Button> &suppressed, bool capture_all) {
    Rig rig;
    rig.engine.configure_side_buttons(true, suppressed, capture_all);
    return rig;
}

Rig hotkey_rig() {
    Rig rig;
    if (!rig.engine.bind_hotkey("clipboard", Modifiers::WIN, KEY_V)) qFatal("bind failed");
    return rig;
}

Rig capture_rig() {
    Rig rig = hotkey_rig();
    rig.engine.configure_key_capture(true);
    return rig;
}

std::string describe(const Events &events) {
    std::string out = "[";
    for (const Event &e : events) {
        out += " kind=" + std::to_string(static_cast<int>(e.kind)) + " id=" + std::to_string(e.id) + " x=" + std::to_string(e.x)
            + " y=" + std::to_string(e.y) + " text=" + e.text + " vk=" + std::to_string(e.vk) + ";";
    }
    return out + " ]";
}

bool kinds(const Events &events, std::initializer_list<Event::Kind> expected) {
    if (events.size() != expected.size()) return false;
    auto it = expected.begin();
    for (const Event &e : events) {
        if (e.kind != *it++) return false;
    }
    return true;
}
}

#define CHECK_RESULT(actual, suppress, ...)                                                                                     \
    do {                                                                                                                        \
        const Result result_ = (actual);                                                                                        \
        const Events expected_{__VA_ARGS__};                                                                                    \
        QVERIFY2(result_.first == (suppress), "swallow decision");                                                             \
        QVERIFY2(result_.second == expected_, ("got " + describe(result_.second) + " expected " + describe(expected_)).c_str()); \
    } while (false)

#define CHECK_EVENTS(actual, ...)                                                                                     \
    do {                                                                                                              \
        const Events actual_ = (actual);                                                                              \
        const Events expected_{__VA_ARGS__};                                                                          \
        QVERIFY2(actual_ == expected_, ("got " + describe(actual_) + " expected " + describe(expected_)).c_str()); \
    } while (false)

// Ported from the Rust state machine's tests when it moved to C++.
class InputEngineTests : public QObject {
    Q_OBJECT
private slots:
    // ------------------------------------------------------------ gesture start and finish

    void a_gesture_starts_only_when_held_modifiers_match_exactly() {
        Rig rig({{ctrl(), Button::Left}});
        rig.hold(LCTRL);
        rig.hold(LALT);
        CHECK_RESULT(rig.mouse(MouseInput::down(Button::Left, 0, 0)), false);
        rig.release(LALT);
        rig.hold(RCTRL);
        CHECK_RESULT(rig.mouse(MouseInput::down(Button::Left, 1, 2)), true, start(1, 1, 2));
    }

    void an_unbound_button_or_no_modifier_passes_through() {
        Rig rig({{ctrl(), Button::Left}});
        CHECK_RESULT(rig.mouse(MouseInput::down(Button::Left, 0, 0)), false);
        CHECK_RESULT(rig.mouse(MouseInput::up(Button::Left, 0, 0)), false);
        rig.hold(LCTRL);
        CHECK_RESULT(rig.mouse(MouseInput::down(Button::Right, 0, 0)), false);
    }

    void press_and_release_are_swallowed_as_a_pair_and_release_finishes() {
        Rig rig({{ctrl(), Button::Left}});
        const uint64_t id = rig.start_ctrl_left();
        rig.release(LCTRL);
        CHECK_RESULT(rig.mouse(MouseInput::up(Button::Left, 30, 40)), true, finish(id, 30, 40));
        QVERIFY(!rig.engine.dragging());
        CHECK_RESULT(rig.mouse(MouseInput::up(Button::Left, 0, 0)), false);
    }

    void every_button_can_start_its_own_binding() {
        for (Button button : ALL_BUTTONS) {
            Rig rig({{ctrl_alt(), button}});
            rig.hold(LCTRL);
            rig.hold(RALT);
            const auto [suppress, events] = rig.mouse(MouseInput::down(button, 0, 0));
            QVERIFY2(suppress, button_name(button));
            QVERIFY2(kinds(events, {Event::Kind::GestureStart}), button_name(button));
        }
    }

    // ------------------------------------------------------------ while dragging

    void moves_pass_through_and_notify_once_until_taken() {
        Rig rig({{ctrl(), Button::Left}});
        const uint64_t id = rig.start_ctrl_left();
        CHECK_RESULT(rig.mouse(MouseInput::move(11, 21)), false, Event::moved(id));
        CHECK_RESULT(rig.mouse(MouseInput::move(12, 22)), false);
        QVERIFY(!rig.engine.take_position(id + 1));
        QCOMPARE(rig.engine.take_position(id), std::make_optional(std::make_pair(12, 22)));
        QVERIFY(!rig.engine.take_position(id));
        CHECK_RESULT(rig.mouse(MouseInput::move(13, 23)), false, Event::moved(id));
    }

    void moves_outside_a_gesture_do_not_notify() {
        Rig rig({{ctrl(), Button::Left}});
        CHECK_RESULT(rig.mouse(MouseInput::move(1, 1)), false);
    }

    void the_wheel_is_swallowed_only_while_dragging() {
        Rig rig({{ctrl(), Button::Left}});
        const MouseInput wheel = MouseInput::wheel(-120, false, 0, 0);
        CHECK_RESULT(rig.mouse(wheel), false);
        rig.start_ctrl_left();
        CHECK_RESULT(rig.mouse(wheel), true);
    }

    void other_clicks_during_a_drag_are_swallowed_with_their_release() {
        Rig rig({{ctrl(), Button::Left}});
        const uint64_t id = rig.start_ctrl_left();
        CHECK_RESULT(rig.mouse(MouseInput::down(Button::Right, 0, 0)), true);
        CHECK_RESULT(rig.mouse(MouseInput::up(Button::Left, 5, 5)), true, finish(id, 5, 5));
        // The right button released after the gesture is still swallowed, so nothing underneath gets a lone release.
        CHECK_RESULT(rig.mouse(MouseInput::up(Button::Right, 0, 0)), true);
        CHECK_RESULT(rig.mouse(MouseInput::up(Button::Right, 0, 0)), false);
    }

    // ------------------------------------------------------------ cancelling

    void escape_cancels_and_both_its_press_and_release_are_swallowed() {
        Rig rig({{ctrl(), Button::Left}});
        const uint64_t id = rig.start_ctrl_left();
        CHECK_RESULT(rig.key(VK_ESCAPE_KEY, true), true, cancel(id, 10, 20));
        // Its auto-repeat too.
        CHECK_RESULT(rig.key(VK_ESCAPE_KEY, true), true);
        CHECK_RESULT(rig.key(VK_ESCAPE_KEY, false), true);
        CHECK_RESULT(rig.key(VK_ESCAPE_KEY, true), false);
    }

    void escape_outside_a_gesture_passes_through() {
        Rig rig({{ctrl(), Button::Left}});
        CHECK_RESULT(rig.key(VK_ESCAPE_KEY, true), false);
        CHECK_RESULT(rig.key(VK_ESCAPE_KEY, false), false);
    }

    void releasing_the_gesture_modifier_cancels_unless_the_other_side_is_held() {
        Rig rig({{ctrl(), Button::Left}});
        const uint64_t id = rig.start_ctrl_left();
        rig.hold(RCTRL);
        CHECK_RESULT(rig.key(LCTRL, false), false);
        QVERIFY(rig.engine.dragging());
        // The real release must reach Windows.
        CHECK_RESULT(rig.key(RCTRL, false), false, cancel(id, 10, 20));
    }

    void releasing_an_unrelated_modifier_keeps_the_drag() {
        Rig rig({{ctrl(), Button::Left}});
        rig.start_ctrl_left();
        rig.hold(LSHIFT);
        CHECK_RESULT(rig.key(LSHIFT, false), false);
        QVERIFY(rig.engine.dragging());
    }

    void cancel_invalidates_queued_events_of_earlier_gestures() {
        Rig rig({{ctrl(), Button::Left}});
        const uint64_t first = rig.start_ctrl_left();
        rig.mouse(MouseInput::up(Button::Left, 0, 0));
        QVERIFY2(rig.engine.accepts(first), "a finished gesture stays valid until something invalidates it");
        // Nothing is dragging, so no cancel event.
        CHECK_EVENTS(rig.with([](Engine &e, Events &out) { e.cancel(out); }));
        QVERIFY(!rig.engine.accepts(first));
        const uint64_t second = rig.start_ctrl_left();
        QVERIFY(rig.engine.accepts(second));
        QVERIFY(!rig.engine.accepts(second + 1));
    }

    // ------------------------------------------------------------ Win/Alt menu masking

    void releasing_win_after_a_win_gesture_masks_the_start_menu_once() {
        Rig rig({{Modifiers::WIN, Button::Left}});
        rig.hold(LWIN);
        const auto [suppress, events] = rig.mouse(MouseInput::down(Button::Left, 0, 0));
        QVERIFY(suppress && kinds(events, {Event::Kind::GestureStart}));
        const uint64_t id = events[0].id;
        rig.mouse(MouseInput::up(Button::Left, 0, 0));
        // Win released after the gesture ended is still masked; the real release passes on.
        CHECK_RESULT(rig.key(LWIN, false), false);
        QCOMPARE(rig.platform.masks, 1);
        QVERIFY(!rig.key(LWIN, false).first);
        QCOMPARE(rig.platform.masks, 1);
        QVERIFY(rig.engine.accepts(id));
    }

    void win_pressed_again_during_the_drag_is_masked_on_release_too() {
        Rig rig({{*Modifiers::parse({"ctrl", "win"}), Button::Left}});
        rig.hold(LCTRL);
        rig.hold(LWIN);
        rig.mouse(MouseInput::down(Button::Left, 0, 0));
        rig.key(RWIN, true);
        rig.key(RWIN, false);
        QCOMPARE(rig.platform.masks, 1);
    }

    void ctrl_gestures_never_mask_menus() {
        Rig rig({{ctrl(), Button::Left}});
        rig.start_ctrl_left();
        rig.hold(LWIN);
        rig.key(LWIN, false);
        QCOMPARE(rig.platform.masks, 0);
    }

    void a_failed_mask_is_reported_without_swallowing_the_release() {
        Rig rig({{Modifiers::ALT, Button::Left}});
        rig.platform.mask_error = "boom";
        rig.hold(LALT);
        rig.mouse(MouseInput::down(Button::Left, 0, 0));
        const auto [suppress, events] = rig.key(LALT, false);
        QVERIFY(!suppress);
        QVERIFY(std::find(events.begin(), events.end(), Event::failure("boom")) != events.end());
    }

    // ------------------------------------------------------------ configuration

    void blocking_prevents_new_gestures_and_cancels_the_current_one() {
        Rig rig({{ctrl(), Button::Left}});
        const uint64_t id = rig.start_ctrl_left();
        CHECK_EVENTS(rig.with([](Engine &e, Events &out) { e.set_blocked(true, out); }), cancel(id, 10, 20));
        // The swallowed press still has its release swallowed.
        QVERIFY(rig.mouse(MouseInput::up(Button::Left, 0, 0)).first);
        CHECK_RESULT(rig.mouse(MouseInput::down(Button::Left, 0, 0)), false);
        rig.with([](Engine &e, Events &out) { e.set_blocked(false, out); });
        QVERIFY(rig.mouse(MouseInput::down(Button::Left, 0, 0)).first);
    }

    void reconfiguring_cancels_only_when_bindings_change_or_disable() {
        Rig rig({{ctrl(), Button::Left}});
        rig.start_ctrl_left();
        CHECK_EVENTS(rig.with([](Engine &e, Events &out) { e.configure_gestures({{ctrl(), Button::Left}}, true, out); }));
        QVERIFY(rig.engine.dragging());
        const Events changed = rig.with([](Engine &e, Events &out) { e.configure_gestures({{ctrl(), Button::Right}}, true, out); });
        QVERIFY(kinds(changed, {Event::Kind::GestureCancel}));
    }

    void bindings_need_one_or_two_modifiers() {
        Engine engine;
        Events ignored;
        QVERIFY(!engine.configure_gestures({}, true, ignored));
        QVERIFY(!engine.configure_gestures({{Modifiers(), Button::Left}}, true, ignored));
        QVERIFY(!engine.configure_gestures({{*Modifiers::parse({"ctrl", "alt", "shift"}), Button::Left}}, true, ignored));
        QVERIFY2(!engine.hooks_needed(), "an invalid configuration never enables the hooks");
        QVERIFY(engine.configure_gestures({{ctrl_alt(), Button::Middle}}, true, ignored));
        QVERIFY(engine.hooks_needed());
        QVERIFY(!Modifiers::parse({"ctrl", "hyper"}));
    }

    void the_binding_of_only_the_current_gesture_is_reported() {
        Rig rig({{ctrl(), Button::Left}});
        const uint64_t id = rig.start_ctrl_left();
        QVERIFY(rig.engine.gesture_binding(id) == std::make_optional(std::make_pair(ctrl(), Button::Left)));
        QVERIFY(!rig.engine.gesture_binding(id - 1));
    }

    // ------------------------------------------------------------ simulated input

    void injected_input_drives_gestures_and_escape_cancels() {
        Rig rig({{ctrl(), Button::Left}});
        rig.hold(LCTRL);
        const auto [swallowed, events] = rig.mouse(injected(MouseInput::down(Button::Left, 0, 0)));
        QVERIFY(swallowed);
        QCOMPARE(events.size(), size_t(1));
        const uint64_t id = rig.engine.gesture_id();
        QVERIFY(rig.engine.accepts(id) && rig.engine.dragging());
        QVERIFY(rig.raw_key(KeyInput{VK_ESCAPE_KEY, true, true}).first);
        QVERIFY(!rig.engine.accepts(id) && !rig.engine.dragging());
    }

    // ------------------------------------------------------------ side-button hotkeys

    void side_buttons_are_reported_and_only_bound_ones_are_swallowed() {
        Rig rig = side_rig({Button::X1}, false);
        CHECK_RESULT(rig.mouse(MouseInput::down(Button::X1, 0, 0)), true, Event::side_button(Button::X1));
        CHECK_RESULT(rig.mouse(MouseInput::up(Button::X1, 0, 0)), true);
        CHECK_RESULT(rig.mouse(MouseInput::down(Button::X2, 0, 0)), false, Event::side_button(Button::X2));
        CHECK_RESULT(rig.mouse(MouseInput::up(Button::X2, 0, 0)), false);
        CHECK_RESULT(rig.mouse(MouseInput::down(Button::Middle, 0, 0)), false);
    }

    void recording_swallows_every_side_button() {
        Rig rig = side_rig({}, true);
        QVERIFY(rig.mouse(MouseInput::down(Button::X2, 0, 0)).first);
        QVERIFY(rig.mouse(MouseInput::up(Button::X2, 0, 0)).first);
    }

    void a_swallowed_side_press_keeps_its_release_after_unbinding() {
        Rig rig = side_rig({Button::X1}, false);
        rig.mouse(MouseInput::down(Button::X1, 0, 0));
        rig.engine.configure_side_buttons(false, {}, false);
        QVERIFY2(rig.engine.hooks_needed(), "the pending release keeps the hook");
        CHECK_RESULT(rig.mouse(MouseInput::up(Button::X1, 0, 0)), true);
        QVERIFY(!rig.engine.hooks_needed());
    }

    void disabled_side_buttons_pass_through_silently() {
        Rig rig = side_rig({Button::X1}, false);
        rig.engine.configure_side_buttons(false, {Button::X1}, false);
        CHECK_RESULT(rig.mouse(MouseInput::down(Button::X1, 0, 0)), false);
    }

    void injected_side_buttons_still_count() {
        // Mouse driver software sends remapped buttons as simulated input.
        Rig rig = side_rig({Button::X1}, false);
        CHECK_RESULT(rig.mouse(injected(MouseInput::down(Button::X1, 0, 0))), true, Event::side_button(Button::X1));
    }

    void a_matching_gesture_takes_precedence_over_the_side_button_hotkey() {
        Rig rig({{ctrl(), Button::X1}});
        rig.engine.configure_side_buttons(true, {Button::X1}, false);
        rig.hold(LCTRL);
        const auto [suppress, events] = rig.mouse(MouseInput::down(Button::X1, 0, 0));
        QVERIFY(suppress && kinds(events, {Event::Kind::GestureStart}));
        rig.release(LCTRL);
        rig.mouse(MouseInput::up(Button::X1, 0, 0));
        CHECK_RESULT(rig.mouse(MouseInput::down(Button::X1, 0, 0)), true, Event::side_button(Button::X1));
    }

    // ------------------------------------------------------------ keyboard hotkeys

    void a_hotkey_swallows_press_repeats_and_release_and_reports_once() {
        Rig rig = hotkey_rig();
        rig.key(LWIN, true);
        CHECK_RESULT(rig.key(KEY_V, true), true, clipboard_hotkey());
        CHECK_RESULT(rig.key(KEY_V, true), true);  // auto-repeat
        CHECK_RESULT(rig.key(KEY_V, false), true);
        CHECK_RESULT(rig.key(KEY_V, true), true, clipboard_hotkey());
        rig.key(KEY_V, false);
    }

    void releasing_win_after_a_hotkey_masks_the_start_menu_without_swallowing() {
        Rig rig = hotkey_rig();
        rig.key(RWIN, true);
        rig.key(KEY_V, true);
        rig.key(KEY_V, false);
        QCOMPARE(rig.platform.masks, 0);
        CHECK_RESULT(rig.key(RWIN, false), false);
        QCOMPARE(rig.platform.masks, 1);
    }

    void releasing_win_before_the_key_still_masks_and_swallows_the_release() {
        Rig rig = hotkey_rig();
        rig.key(LWIN, true);
        rig.key(KEY_V, true);
        QVERIFY(!rig.key(LWIN, false).first);
        QCOMPARE(rig.platform.masks, 1);
        CHECK_RESULT(rig.key(KEY_V, false), true);
        QVERIFY2(!rig.key(KEY_V, true).first, "plain V after Win is released");
    }

    void hotkeys_need_exactly_their_modifiers() {
        Rig rig = hotkey_rig();
        CHECK_RESULT(rig.key(KEY_V, true), false);
        rig.key(KEY_V, false);
        rig.key(LWIN, true);
        rig.key(LSHIFT, true);
        CHECK_RESULT(rig.key(KEY_V, true), false);
        rig.key(KEY_V, false);
        rig.key(LWIN, false);
        QVERIFY2(rig.platform.masks == 0, "nothing was swallowed");
    }

    void a_key_held_before_the_modifiers_is_left_alone() {
        Rig rig = hotkey_rig();
        rig.key(KEY_V, true);
        rig.key(LWIN, true);
        CHECK_RESULT(rig.key(KEY_V, true), false);  // auto-repeat
        QVERIFY(!rig.key(KEY_V, false).first);
    }

    void injected_keys_trigger_hotkeys_once_and_swallow_release() {
        Rig rig = hotkey_rig();
        rig.key(LWIN, true);
        const KeyInput press{KEY_V, true, true};
        CHECK_RESULT(rig.raw_key(press), true, clipboard_hotkey());
        CHECK_RESULT(rig.raw_key(press), true);
        CHECK_RESULT(rig.raw_key(KeyInput{KEY_V, false, true}), true);
    }

    void unbinding_keeps_swallowing_the_pending_release() {
        Rig rig = hotkey_rig();
        rig.key(LWIN, true);
        rig.key(KEY_V, true);
        rig.key(LWIN, false);
        rig.engine.unbind_hotkey("clipboard");
        QVERIFY(rig.engine.hooks_needed());
        QVERIFY(rig.key(KEY_V, false).first);
        QVERIFY(!rig.engine.hooks_needed());
        rig.key(LWIN, true);
        CHECK_RESULT(rig.key(KEY_V, true), false);
    }

    void hotkeys_need_a_modifier_and_a_non_modifier_key() {
        Engine engine;
        QVERIFY(!engine.bind_hotkey("bare", Modifiers(), KEY_V));
        QVERIFY(!engine.bind_hotkey("modifier", Modifiers::WIN, LSHIFT));
        QVERIFY(!engine.bind_hotkey("generic", Modifiers::WIN, 0x10));
        QVERIFY(!engine.hooks_needed());
        QVERIFY(engine.bind_hotkey("clipboard", Modifiers::WIN, KEY_V));
        QVERIFY(engine.hooks_needed());
        QVERIFY(!engine.bind_hotkey("clipboard", Modifiers(), KEY_V));
        QVERIFY2(!engine.hooks_needed(), "an invalid rebind removes the old one");
    }

    void print_screen_is_the_one_key_that_binds_without_modifiers() {
        Rig rig;
        QVERIFY(rig.engine.bind_hotkey("capture", Modifiers(), VK_PRINT_SCREEN));
        CHECK_RESULT(rig.key(VK_PRINT_SCREEN, true), true, Event::hotkey("capture"));
        CHECK_RESULT(rig.key(VK_PRINT_SCREEN, false), true);
        rig.key(LSHIFT, true);
        CHECK_RESULT(rig.key(VK_PRINT_SCREEN, true), false);  // Shift+PrtSc is another combination
        rig.key(VK_PRINT_SCREEN, false);
        rig.key(LSHIFT, false);
        QVERIFY(!rig.engine.bind_hotkey("bare", Modifiers(), KEY_V));
    }

    void recording_swallows_win_combinations_and_reports_them_once() {
        Rig rig = capture_rig();
        const Modifiers win_shift = *Modifiers::parse({"win", "shift"});
        CHECK_RESULT(rig.key(LWIN, true), false);  // Win itself reaches the edit
        rig.key(LSHIFT, true);
        CHECK_RESULT(rig.key(KEY_S, true), true, Event::captured(win_shift, KEY_S));
        CHECK_RESULT(rig.key(KEY_S, true), true);  // auto-repeat
        CHECK_RESULT(rig.key(KEY_S, false), true);
        rig.key(LSHIFT, false);
        QVERIFY(!rig.key(LWIN, false).first);
        QVERIFY2(rig.platform.masks == 1, "the Start menu stays closed");
    }

    void recording_lets_the_menu_mask_through() {
        Rig rig = capture_rig();
        rig.key(LWIN, true);
        rig.key(KEY_S, true);
        rig.key(KEY_S, false);
        // The pair the hook injects before passing on the Win release.
        CHECK_RESULT(rig.key(VK_MENU_MASK, true), false);
        CHECK_RESULT(rig.key(VK_MENU_MASK, false), false);
        QVERIFY(!rig.key(LWIN, false).first);
    }

    void recording_takes_precedence_over_a_bound_hotkey() {
        Rig rig = capture_rig();
        rig.key(LWIN, true);
        CHECK_RESULT(rig.key(KEY_V, true), true, Event::captured(Modifiers::WIN, KEY_V));
        rig.key(KEY_V, false);
        rig.key(LWIN, false);
        rig.engine.configure_key_capture(false);
        rig.key(LWIN, true);
        CHECK_RESULT(rig.key(KEY_V, true), true, clipboard_hotkey());
    }

    void recording_takes_print_screen_with_or_without_modifiers() {
        Rig rig = capture_rig();
        CHECK_RESULT(rig.key(VK_PRINT_SCREEN, true), true, Event::captured(Modifiers(), VK_PRINT_SCREEN));
        rig.key(VK_PRINT_SCREEN, false);
        rig.key(LCTRL, true);
        CHECK_RESULT(rig.key(VK_PRINT_SCREEN, true), true, Event::captured(ctrl(), VK_PRINT_SCREEN));
        rig.key(VK_PRINT_SCREEN, false);
        QVERIFY2(rig.platform.masks == 0, "no Win, no menu to mask");
    }

    void recording_leaves_keys_the_edit_receives_itself() {
        Rig rig = capture_rig();
        CHECK_RESULT(rig.key(KEY_V, true), false);
        rig.key(KEY_V, false);
        rig.key(LCTRL, true);
        rig.key(LSHIFT, true);
        CHECK_RESULT(rig.key(KEY_V, true), false);  // Ctrl+Shift+V comes through Qt
        rig.key(KEY_V, false);
    }

    void recording_skips_a_key_held_before_win() {
        Rig rig = capture_rig();
        rig.key(KEY_V, true);
        rig.key(LWIN, true);
        CHECK_RESULT(rig.key(KEY_V, true), false);  // auto-repeat
        QVERIFY(!rig.key(KEY_V, false).first);
    }

    void ending_recording_keeps_swallowing_the_pending_release() {
        Rig rig = capture_rig();
        QVERIFY(rig.engine.hooks_needed());
        rig.key(LWIN, true);
        rig.key(KEY_J, true);
        rig.engine.configure_key_capture(false);
        rig.engine.unbind_hotkey("clipboard");
        rig.key(LWIN, false);
        QVERIFY2(rig.engine.hooks_needed(), "the release of J is still owed");
        QVERIFY(rig.key(KEY_J, false).first);
        QVERIFY(!rig.engine.hooks_needed());
    }

    void a_failure_ends_recording() {
        Rig rig = capture_rig();
        rig.key(LWIN, true);
        rig.key(KEY_J, true);
        rig.with([](Engine &e, Events &out) { e.fail("hook lost", out); });
        QVERIFY(!rig.engine.hooks_needed());
        QVERIFY(!rig.key(KEY_J, false).first);
    }

    void a_gesture_swallowing_escape_takes_precedence_over_a_hotkey() {
        Rig rig({{ctrl(), Button::Left}});
        QVERIFY(rig.engine.bind_hotkey("esc", ctrl(), VK_ESCAPE_KEY));
        rig.start_ctrl_left();
        const auto [suppress, events] = rig.key(VK_ESCAPE_KEY, true);
        QVERIFY(suppress && kinds(events, {Event::Kind::GestureCancel}));
        rig.key(VK_ESCAPE_KEY, false);
        CHECK_RESULT(rig.key(VK_ESCAPE_KEY, true), true, Event::hotkey("esc"));
    }

    void a_failure_drops_hotkeys_and_their_claims() {
        Rig rig = hotkey_rig();
        rig.key(LWIN, true);
        rig.key(KEY_V, true);
        rig.with([](Engine &e, Events &out) { e.fail("hook lost", out); });
        QVERIFY(!rig.engine.hooks_needed());
        QVERIFY(!rig.key(KEY_V, false).first);
        CHECK_RESULT(rig.key(KEY_V, true), false);
    }

    // ------------------------------------------------------------ watchers

    void wheel_watchers_hear_only_their_area_and_never_swallow() {
        Rig rig;
        rig.engine.watch_wheel("stitch", Rect{0, 0, 100, 100});
        rig.engine.watch_wheel("gif", std::nullopt);
        const auto [suppress, events] = rig.mouse(MouseInput::wheel(-240, false, 50, 50));
        QVERIFY(!suppress);
        QCOMPARE(events.size(), size_t(2));
        CHECK_RESULT(rig.mouse(MouseInput::wheel(-240, false, 150, 50)), false, Event::wheel("gif", 150, 50, -240, false));
        rig.engine.unwatch_wheel("gif");
        CHECK_RESULT(rig.mouse(MouseInput::wheel(-240, false, 150, 50)), false);
    }

    void injected_wheel_events_are_heard() {
        Rig rig;
        rig.engine.watch_wheel("stitch", std::nullopt);
        QCOMPARE(rig.mouse(injected(MouseInput::wheel(120, true, 0, 0))).second.size(), size_t(1));
    }

    void key_watchers_hear_presses_and_releases_without_swallowing() {
        Rig rig;
        rig.engine.watch_keys("stitch", {0x10, LSHIFT});
        CHECK_RESULT(rig.key(LSHIFT, true), false, Event::key("stitch", LSHIFT, true));
        QCOMPARE(rig.key(LSHIFT, false).second.size(), size_t(1));
        CHECK_RESULT(rig.key(0x41, true), false);
    }

    // The pointer's light: every button, and one notice for moves until the position is collected.
    void mouse_watchers_hear_buttons_and_one_move_until_collected() {
        Rig rig;
        rig.engine.watch_mouse("light");
        CHECK_RESULT(rig.mouse(MouseInput::move(5, 6)), false, Event::pointer_moved("light"));
        // Quiet until the UI collects the latest position, however many moves come.
        CHECK_RESULT(rig.mouse(MouseInput::move(7, 8)), false);
        CHECK_RESULT(rig.mouse(MouseInput::move(9, 10)), false);
        const auto position = rig.engine.take_pointer("light");
        QVERIFY(position && *position == std::make_pair(9, 10));
        QVERIFY2(!rig.engine.take_pointer("light"), "nothing to collect twice");
        CHECK_RESULT(rig.mouse(MouseInput::move(11, 12)), false, Event::pointer_moved("light"));
        CHECK_RESULT(rig.mouse(MouseInput::down(Button::Left, 11, 12)), false, Event::mouse_button("light", Button::Left, true, 11, 12));
        CHECK_RESULT(rig.mouse(MouseInput::up(Button::Left, 11, 12)), false, Event::mouse_button("light", Button::Left, false, 11, 12));
        CHECK_RESULT(rig.mouse(MouseInput::down(Button::Right, 3, 4)), false, Event::mouse_button("light", Button::Right, true, 3, 4));
        CHECK_RESULT(rig.mouse(MouseInput::down(Button::Middle, 3, 4)), false, Event::mouse_button("light", Button::Middle, true, 3, 4));
    }

    void each_mouse_watcher_has_its_own_pending_move() {
        Rig rig;
        rig.engine.watch_mouse("a");
        rig.engine.watch_mouse("b");
        CHECK_RESULT(rig.mouse(MouseInput::move(1, 2)), false, Event::pointer_moved("a"), Event::pointer_moved("b"));
        QVERIFY(rig.engine.take_pointer("a"));
        CHECK_RESULT(rig.mouse(MouseInput::move(3, 4)), false, Event::pointer_moved("a"));
        QVERIFY(!rig.engine.take_pointer("nobody"));
    }

    void mouse_watchers_hear_injected_input_and_ignore_the_wheel() {
        Rig rig;
        rig.engine.watch_mouse("light");
        CHECK_RESULT(rig.mouse(injected(MouseInput::down(Button::Left, 1, 1))), false, Event::mouse_button("light", Button::Left, true, 1, 1));
        CHECK_RESULT(rig.mouse(MouseInput::wheel(120, false, 1, 1)), false);
    }

    void a_press_a_gesture_swallows_is_not_reported_to_mouse_watchers() {
        Rig rig({{ctrl(), Button::Left}});
        rig.engine.watch_mouse("light");
        rig.hold(LCTRL);
        const auto [swallowed, events] = rig.mouse(MouseInput::down(Button::Left, 10, 20));
        QVERIFY(swallowed && kinds(events, {Event::Kind::GestureStart}));
        // The drag's moves still reach the watcher.
        const auto [swallow_move, moves] = rig.mouse(MouseInput::move(30, 40));
        QVERIFY(!swallow_move && kinds(moves, {Event::Kind::GestureMoved, Event::Kind::PointerMoved}));
        const auto [swallowed_up, up] = rig.mouse(MouseInput::up(Button::Left, 30, 40));
        QVERIFY(swallowed_up && kinds(up, {Event::Kind::GestureFinish}));
        // A button no gesture takes is heard.
        CHECK_RESULT(rig.mouse(MouseInput::down(Button::Right, 30, 40)), false, Event::mouse_button("light", Button::Right, true, 30, 40));
    }

    void side_buttons_are_reported_to_mouse_watchers_unless_swallowed() {
        Rig rig = side_rig({Button::X2}, false);
        rig.engine.watch_mouse("light");
        CHECK_RESULT(rig.mouse(MouseInput::down(Button::X1, 5, 5)), false, Event::side_button(Button::X1), Event::mouse_button("light", Button::X1, true, 5, 5));
        CHECK_RESULT(rig.mouse(MouseInput::down(Button::X2, 5, 5)), true, Event::side_button(Button::X2));
        CHECK_RESULT(rig.mouse(MouseInput::up(Button::X2, 5, 5)), true);
    }

    void a_mouse_watcher_alone_needs_the_hooks_and_ends_with_unwatch_or_close() {
        Engine engine;
        QVERIFY(!engine.hooks_needed());
        engine.watch_mouse("light");
        QVERIFY(engine.hooks_needed());
        QVERIFY(engine.take_sync_request());
        engine.unwatch_mouse("light");
        QVERIFY(!engine.hooks_needed());
        QVERIFY(engine.take_sync_request());
        engine.watch_mouse("light");
        engine.watch_mouse("light");
        engine.unwatch_mouse("light");
        QVERIFY2(!engine.hooks_needed(), "watching twice is still one watcher");
        engine.watch_mouse("light");
        Events events;
        engine.close(events);
        QVERIFY(!engine.hooks_needed());
        FakePlatform platform;
        QVERIFY(!engine.on_mouse(MouseInput::move(1, 1), platform, events));
        QVERIFY(events.empty());
    }

    void foreground_changes_are_reported_once_while_anyone_watches() {
        Engine engine;
        Events events;
        engine.on_foreground(7, events);
        QVERIFY(events.empty());
        QVERIFY(!engine.foreground_needed());
        engine.watch_foreground("clipboard");
        engine.watch_foreground("clipboard");
        engine.watch_foreground("other");
        QVERIFY(engine.foreground_needed());
        QVERIFY2(!engine.hooks_needed(), "foreground events need no low-level hooks");
        engine.on_foreground(7, events);
        CHECK_EVENTS(events, Event::foreground(7));
        engine.unwatch_foreground("clipboard");
        QVERIFY(engine.foreground_needed());
        engine.unwatch_foreground("other");
        QVERIFY(!engine.foreground_needed());
        engine.watch_foreground("clipboard");
        Events ignored;
        engine.close(ignored);
        QVERIFY(!engine.foreground_needed());
    }

    // ------------------------------------------------------------ lifetime

    void hooks_are_needed_only_while_something_listens_or_waits_for_a_release() {
        Rig rig({{ctrl(), Button::Left}});
        rig.start_ctrl_left();
        rig.with([](Engine &e, Events &out) { e.configure_gestures({{ctrl(), Button::Left}}, false, out); });
        QVERIFY2(rig.engine.hooks_needed(), "the swallowed press still waits for its release");
        rig.mouse(MouseInput::up(Button::Left, 0, 0));
        QVERIFY(!rig.engine.hooks_needed());
        QVERIFY(rig.engine.take_sync_request());
        QVERIFY(!rig.engine.take_sync_request());
        rig.engine.watch_keys("stitch", {0x10});
        QVERIFY(rig.engine.hooks_needed());
    }

    void a_failure_disables_everything_and_releases_claims() {
        Rig rig({{ctrl(), Button::Left}});
        rig.engine.configure_side_buttons(true, {Button::X2}, false);
        const uint64_t id = rig.start_ctrl_left();
        CHECK_EVENTS(rig.with([](Engine &e, Events &out) { e.fail("hook lost", out); }), cancel(id, 10, 20),
            Event::failure("hook lost"));
        QVERIFY(!rig.engine.hooks_needed());
        CHECK_RESULT(rig.mouse(MouseInput::up(Button::Left, 0, 0)), false);
    }

    void a_closed_engine_ignores_everything() {
        Rig rig({{ctrl(), Button::Left}});
        const uint64_t id = rig.start_ctrl_left();
        CHECK_EVENTS(rig.with([](Engine &e, Events &out) { e.close(out); }), cancel(id, 10, 20));
        QVERIFY(!rig.engine.accepts(id));
        QVERIFY(!rig.engine.hooks_needed());
        CHECK_RESULT(rig.mouse(MouseInput::down(Button::Left, 0, 0)), false);
        rig.with([](Engine &e, Events &out) { e.configure_gestures({{ctrl(), Button::Left}}, true, out); });
        QVERIFY2(!rig.engine.hooks_needed(), "a closed engine cannot be re-enabled");
    }
};

QTEST_GUILESS_MAIN(InputEngineTests)
#include "test_input_engine.moc"
