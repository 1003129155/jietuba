#include "core/input_hooks.h"
#include <QTest>
#include <algorithm>
#include <memory>
#include <windows.h>

using namespace jietuba::input;

// The hook thread on the real desktop, driven by injected input. An observer hook installed
// before the hub's hooks runs after them, so it sees exactly what the hub lets through.
namespace {
constexpr ULONG_PTR TEST_MARKER = 0x4A494854;
constexpr WORD KEY_F24 = VK_F24;

struct Seen {
    DWORD vk;
    bool down;
};
std::vector<Seen> seen_keys;
std::vector<WPARAM> seen_mouse;

LRESULT CALLBACK observe_keys(int code, WPARAM wparam, LPARAM lparam) {
    if (code >= 0) {
        const auto *data = reinterpret_cast<const KBDLLHOOKSTRUCT *>(lparam);
        seen_keys.push_back({data->vkCode, wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN});
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

LRESULT CALLBACK observe_mouse(int code, WPARAM wparam, LPARAM lparam) {
    if (code >= 0 && wparam != WM_MOUSEMOVE) seen_mouse.push_back(wparam);
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

void send_key(WORD vk, bool down) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    input.ki.dwExtraInfo = TEST_MARKER;
    SendInput(1, &input, sizeof input);
}

void send_x_button(DWORD button, bool down) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.mouseData = button;
    input.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
    input.mi.dwExtraInfo = TEST_MARKER;
    SendInput(1, &input, sizeof input);
}

bool saw_key(DWORD vk) {
    return std::any_of(seen_keys.begin(), seen_keys.end(), [vk](const Seen &s) { return s.vk == vk; });
}

size_t count_kind(const Events &events, Event::Kind kind) {
    return static_cast<size_t>(std::count_if(events.begin(), events.end(), [kind](const Event &e) { return e.kind == kind; }));
}
}

class InputHooksTests : public QObject {
    Q_OBJECT
private slots:
    void init() {
        seen_keys.clear();
        seen_mouse.clear();
        observer_keys_ = SetWindowsHookExW(WH_KEYBOARD_LL, observe_keys, GetModuleHandleW(nullptr), 0);
        observer_mouse_ = SetWindowsHookExW(WH_MOUSE_LL, observe_mouse, GetModuleHandleW(nullptr), 0);
        QVERIFY(observer_keys_ && observer_mouse_);
        hooks_ = std::make_unique<InputHooks>();
        hooks_->set_test_marker(TEST_MARKER);
    }

    void cleanup() {
        // Nothing may stay held after a failed case.
        for (WORD vk : {KEY_F24, WORD(VK_LCONTROL), WORD(VK_LWIN)}) send_key(vk, false);
        hooks_.reset();
        UnhookWindowsHookEx(observer_keys_);
        UnhookWindowsHookEx(observer_mouse_);
    }

    void only_one_hub_exists_per_process() {
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, InputHooks());
        hooks_.reset();
        InputHooks again;
        QVERIFY(!again.hooks_installed());
    }

    void hooks_install_only_while_a_feature_needs_them() {
        QVERIFY(!hooks_->hooks_installed());
        hooks_->with_engine([](Engine &engine, Events &) { engine.watch_foreground("tracker"); });
        QTest::qWait(100);
        QVERIFY2(!hooks_->hooks_installed(), "foreground events need no low-level hooks");
        hooks_->with_engine([](Engine &engine, Events &) { engine.bind_hotkey("t", Modifiers::CTRL, KEY_F24); });
        QTRY_VERIFY(hooks_->hooks_installed());
        hooks_->with_engine([](Engine &engine, Events &) { engine.unbind_hotkey("t"); });
        QTRY_VERIFY(!hooks_->hooks_installed());
    }

    void a_hotkey_is_swallowed_with_its_repeat_and_release_and_reported_once() {
        hooks_->with_engine([](Engine &engine, Events &) { engine.bind_hotkey("t", Modifiers::CTRL, KEY_F24); });
        QTRY_VERIFY(hooks_->hooks_installed());
        send_key(VK_LCONTROL, true);
        send_key(KEY_F24, true);
        send_key(KEY_F24, true);
        send_key(KEY_F24, false);
        send_key(VK_LCONTROL, false);
        Events events;
        QTRY_VERIFY(collect(events) && count_kind(events, Event::Kind::Hotkey) == 1);
        // Ctrl's release passed both hooks, so everything injected before it was handled.
        QTRY_VERIFY(std::any_of(seen_keys.begin(), seen_keys.end(), [](const Seen &s) { return s.vk == VK_LCONTROL && !s.down; }));
        collect(events);
        QCOMPARE(count_kind(events, Event::Kind::Hotkey), size_t(1));
        QCOMPARE(events.front().text, std::string("t"));
        QVERIFY2(!saw_key(KEY_F24), "the press, its repeat and the release are swallowed");
    }

    void releasing_win_after_a_hotkey_masks_the_start_menu_before_the_release() {
        hooks_->with_engine([](Engine &engine, Events &) { engine.bind_hotkey("w", Modifiers::WIN, KEY_F24); });
        QTRY_VERIFY(hooks_->hooks_installed());
        send_key(VK_LWIN, true);
        send_key(KEY_F24, true);
        send_key(KEY_F24, false);
        send_key(VK_LWIN, false);
        const auto win_released = [] {
            return std::find_if(seen_keys.begin(), seen_keys.end(), [](const Seen &s) { return s.vk == VK_LWIN && !s.down; });
        };
        QTRY_VERIFY(win_released() != seen_keys.end());
        const auto release = win_released();
        const auto mask_down = std::find_if(seen_keys.begin(), release, [](const Seen &s) { return s.vk == VK_MENU_MASK && s.down; });
        QVERIFY2(mask_down != release, "the mask is sent before Win's release passes on");
        QVERIFY(std::any_of(mask_down, release, [](const Seen &s) { return s.vk == VK_MENU_MASK && !s.down; }));
        QVERIFY(!saw_key(KEY_F24));
        Events events;
        QTRY_VERIFY(collect(events) && count_kind(events, Event::Kind::Hotkey) == 1);
        QCOMPARE(count_kind(events, Event::Kind::Failure), size_t(0));
    }

    void a_suppressed_side_button_is_swallowed_as_a_pair() {
        hooks_->with_engine([](Engine &engine, Events &) { engine.configure_side_buttons(true, {Button::X1}, false); });
        QTRY_VERIFY(hooks_->hooks_installed());
        send_x_button(XBUTTON1, true);
        send_x_button(XBUTTON1, false);
        Events events;
        QTRY_VERIFY(collect(events) && count_kind(events, Event::Kind::SideButton) == 1);
        // A key after the button passes both hooks once the button was handled.
        send_key(KEY_F24, true);
        send_key(KEY_F24, false);
        QTRY_VERIFY(saw_key(KEY_F24));
        QVERIFY2(seen_mouse.empty(), "neither the press nor the release reaches the next hook");
        QCOMPARE(events.front().button, Button::X1);
    }

    void watchers_hear_keys_without_swallowing_them() {
        hooks_->with_engine([](Engine &engine, Events &) { engine.watch_keys("stitch", {KEY_F24}); });
        QTRY_VERIFY(hooks_->hooks_installed());
        send_key(KEY_F24, true);
        send_key(KEY_F24, false);
        Events events;
        QTRY_VERIFY(collect(events) && count_kind(events, Event::Kind::Key) == 2);
        QVERIFY(events[0].pressed && !events[1].pressed);
        QTRY_VERIFY(std::count_if(seen_keys.begin(), seen_keys.end(), [](const Seen &s) { return s.vk == KEY_F24; }) == 2);
    }

    void stopping_drains_the_queued_events_then_ends_the_stream() {
        hooks_->with_engine([](Engine &engine, Events &out) {
            engine.bind_hotkey("t", Modifiers::CTRL, KEY_F24);
            engine.fail("lost", out);
        });
        hooks_->stop();
        const auto first = hooks_->next_event(0);
        QVERIFY(first && first->kind == Event::Kind::Failure && first->text == "lost");
        QVERIFY(!hooks_->next_event(-1));
        QVERIFY(!hooks_->hooks_installed());
        // A closed engine still answers and refuses new work.
        bool bound = true;
        hooks_->with_engine([&](Engine &engine, Events &) { bound = engine.bind_hotkey("t", Modifiers::CTRL, KEY_F24); });
        QVERIFY(!bound);
        hooks_->stop();
    }

private:
    bool collect(Events &events) {
        while (auto event = hooks_->next_event(0)) events.push_back(std::move(*event));
        return true;
    }

    std::unique_ptr<InputHooks> hooks_;
    HHOOK observer_keys_ = nullptr;
    HHOOK observer_mouse_ = nullptr;
};

QTEST_MAIN(InputHooksTests)
#include "test_input_hooks.moc"
