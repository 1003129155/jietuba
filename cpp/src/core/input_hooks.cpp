#include "core/input_hooks.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <windows.h>

namespace jietuba::input {
namespace {
constexpr UINT WM_APP_SYNC = WM_APP + 1;
std::atomic<bool> hooks_exist{false};

struct Win32Platform final : Platform {
    bool mask_requested = false;
    bool key_held(uint32_t vk) const override { return (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0; }
    // Only recorded; State::handle sends it after releasing the engine lock.
    std::optional<std::string> mask_menu() override {
        mask_requested = true;
        return std::nullopt;
    }
};

// An unassigned key pressed and released keeps the next released Win/Alt from opening its
// menu without swallowing the real release.
std::optional<std::string> send_menu_mask() {
    INPUT inputs[2] = {};
    for (INPUT &input : inputs) {
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = static_cast<WORD>(VK_MENU_MASK);
    }
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    if (SendInput(2, inputs, sizeof(INPUT)) == 2) return std::nullopt;
    return "Could not mask the modifier menu (error " + std::to_string(GetLastError()) + ")";
}

LRESULT CALLBACK mouse_proc(int code, WPARAM wparam, LPARAM lparam) noexcept;
LRESULT CALLBACK keyboard_proc(int code, WPARAM wparam, LPARAM lparam) noexcept;
void CALLBACK foreground_proc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG, LONG, DWORD, DWORD) noexcept;
}

struct InputHooks::State {
    std::mutex engine_mutex;
    Engine engine;
    std::mutex queue_mutex;
    std::condition_variable queue_ready;
    std::deque<Event> queue;
    bool queue_closed = false;
    std::atomic<uintptr_t> test_marker{0};
    std::atomic<DWORD> thread_id{0};
    std::atomic<bool> hooks_installed{false};
    std::mutex stop_mutex;
    std::thread thread;
    HHOOK mouse_hook = nullptr;
    HHOOK keyboard_hook = nullptr;
    HWINEVENTHOOK foreground_hook = nullptr;

    // Events are queued before the engine lock is released, so they keep the engine's order
    // across threads.
    template <class F>
    auto with_engine(F &&f) {
        std::unique_lock<std::mutex> lock(engine_mutex);
        Events events;
        if constexpr (std::is_void_v<decltype(f(engine, events))>) {
            f(engine, events);
            finish(lock, events);
        } else {
            auto result = f(engine, events);
            finish(lock, events);
            return result;
        }
    }

    void finish(std::unique_lock<std::mutex> &lock, Events &events) {
        if (!events.empty()) {
            std::lock_guard<std::mutex> queue_lock(queue_mutex);
            if (!queue_closed) {
                for (Event &event : events) queue.push_back(std::move(event));
            }
        }
        const bool sync = engine.take_sync_request();
        lock.unlock();
        if (!events.empty()) queue_ready.notify_one();
        if (sync) request_sync();
    }

    void request_sync() const {
        if (const DWORD id = thread_id.load(std::memory_order_acquire)) PostThreadMessageW(id, WM_APP_SYNC, 0, 0);
    }

    bool counts_as_real(bool injected, ULONG_PTR extra_info) const {
        const uintptr_t marker = test_marker.load(std::memory_order_acquire);
        return !injected || (marker != 0 && extra_info == marker);
    }

    template <class Run>
    bool handle(Run &&run);
    bool handle_mouse(UINT message, const MSLLHOOKSTRUCT &data);
    bool handle_key(UINT message, const KBDLLHOOKSTRUCT &data);
    void run(std::promise<void> &ready);
    void sync_hooks();
    void remove_low_level();
    void remove_foreground();
};

namespace {
// Hook callbacks run on the hook thread and find the state here, without a global lock.
thread_local InputHooks::State *current = nullptr;
// This thread holds the engine lock while handling a hook event.
thread_local bool in_engine = false;
}

template <class Run>
bool InputHooks::State::handle(Run &&run) {
    // The system may re-enter the hook callback from a system call on this thread while the
    // outer call holds the engine lock; waiting for the lock would stall all input in the
    // system, so the nested event passes through.
    if (in_engine) return false;
    Win32Platform platform;
    bool suppress = false;
    {
        in_engine = true;
        struct Reset {
            ~Reset() { in_engine = false; }
        } reset;
        suppress = with_engine([&](Engine &engine, Events &out) {
            try {
                return run(engine, platform, out);
            } catch (...) {
                engine.fail("input hook failed", out);
                return false;
            }
        });
    }
    if (platform.mask_requested) {
        // Sent after unlocking and before this release passes on: SendInput re-enters the hook callback.
        if (auto error = send_menu_mask())
            with_engine([&](Engine &, Events &out) { out.push_back(Event::failure(std::move(*error))); });
    }
    return suppress;
}

bool InputHooks::State::handle_mouse(UINT message, const MSLLHOOKSTRUCT &data) {
    const WORD high_word = HIWORD(data.mouseData);
    MouseInput input;
    switch (message) {
    case WM_MOUSEMOVE: input.action = MouseInput::Action::Move; break;
    case WM_LBUTTONDOWN: input = MouseInput::down(Button::Left, 0, 0); break;
    case WM_LBUTTONUP: input = MouseInput::up(Button::Left, 0, 0); break;
    case WM_RBUTTONDOWN: input = MouseInput::down(Button::Right, 0, 0); break;
    case WM_RBUTTONUP: input = MouseInput::up(Button::Right, 0, 0); break;
    case WM_MBUTTONDOWN: input = MouseInput::down(Button::Middle, 0, 0); break;
    case WM_MBUTTONUP: input = MouseInput::up(Button::Middle, 0, 0); break;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP: {
        // The high word tells the side buttons apart.
        Button button;
        if (high_word == XBUTTON1) button = Button::X1;
        else if (high_word == XBUTTON2) button = Button::X2;
        else return false;
        input = message == WM_XBUTTONDOWN ? MouseInput::down(button, 0, 0) : MouseInput::up(button, 0, 0);
        break;
    }
    case WM_MOUSEWHEEL: input = MouseInput::wheel(static_cast<int16_t>(high_word), false, 0, 0); break;
    case WM_MOUSEHWHEEL: input = MouseInput::wheel(static_cast<int16_t>(high_word), true, 0, 0); break;
    default: return false;
    }
    input.x = data.pt.x;
    input.y = data.pt.y;
    const bool injected = (data.flags & (LLMHF_INJECTED | LLMHF_LOWER_IL_INJECTED)) != 0;
    input.injected = !counts_as_real(injected, data.dwExtraInfo);
    return handle([&](Engine &engine, Platform &platform, Events &out) { return engine.on_mouse(input, platform, out); });
}

bool InputHooks::State::handle_key(UINT message, const KBDLLHOOKSTRUCT &data) {
    bool pressed;
    switch (message) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: pressed = true; break;
    case WM_KEYUP:
    case WM_SYSKEYUP: pressed = false; break;
    default: return false;
    }
    const bool injected = (data.flags & (LLKHF_INJECTED | LLKHF_LOWER_IL_INJECTED)) != 0;
    const KeyInput input{data.vkCode, pressed, !counts_as_real(injected, data.dwExtraInfo)};
    return handle([&](Engine &engine, Platform &platform, Events &out) { return engine.on_key(input, platform, out); });
}

void InputHooks::State::run(std::promise<void> &ready) {
    MSG msg;
    // Creates the message queue first, so sync requests posted from other threads are not lost.
    PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);
    current = this;
    thread_id.store(GetCurrentThreadId(), std::memory_order_release);
    ready.set_value();
    sync_hooks();
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_APP_SYNC) sync_hooks();
    }
    remove_low_level();
    remove_foreground();
    thread_id.store(0, std::memory_order_release);
    current = nullptr;
}

void InputHooks::State::sync_hooks() {
    try {
        const auto [low_level, foreground] = with_engine([](Engine &engine, Events &) {
            return std::make_pair(engine.hooks_needed(), engine.foreground_needed());
        });
        if (low_level && !mouse_hook) {
            const HINSTANCE module = GetModuleHandleW(nullptr);
            HHOOK mouse = SetWindowsHookExW(WH_MOUSE_LL, mouse_proc, module, 0);
            HHOOK keyboard = SetWindowsHookExW(WH_KEYBOARD_LL, keyboard_proc, module, 0);
            if (!mouse || !keyboard) {
                const DWORD error = GetLastError();
                if (mouse) UnhookWindowsHookEx(mouse);
                if (keyboard) UnhookWindowsHookEx(keyboard);
                with_engine([&](Engine &engine, Events &out) {
                    engine.fail("SetWindowsHookExW failed (error " + std::to_string(error) + ")", out);
                });
                return;
            }
            mouse_hook = mouse;
            keyboard_hook = keyboard;
            hooks_installed.store(true, std::memory_order_release);
        } else if (!low_level) {
            remove_low_level();
        }
        if (foreground && !foreground_hook) {
            // This process's windows too: its other windows may also be a paste target.
            foreground_hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, foreground_proc,
                0, 0, WINEVENT_OUTOFCONTEXT);
            if (!foreground_hook) {
                const std::string message = "SetWinEventHook failed (error " + std::to_string(GetLastError()) + ")";
                with_engine([&](Engine &, Events &out) { out.push_back(Event::failure(message)); });
            }
        } else if (!foreground) {
            remove_foreground();
        }
    } catch (...) {
        // Out of memory; the hooks stay as they are until the next request.
    }
}

void InputHooks::State::remove_low_level() {
    if (mouse_hook) {
        UnhookWindowsHookEx(mouse_hook);
        UnhookWindowsHookEx(keyboard_hook);
        mouse_hook = keyboard_hook = nullptr;
    }
    hooks_installed.store(false, std::memory_order_release);
}

void InputHooks::State::remove_foreground() {
    if (foreground_hook) {
        UnhookWinEvent(foreground_hook);
        foreground_hook = nullptr;
    }
}

namespace {
LRESULT CALLBACK mouse_proc(int code, WPARAM wparam, LPARAM lparam) noexcept {
    if (code >= 0 && current) {
        try {
            if (current->handle_mouse(static_cast<UINT>(wparam), *reinterpret_cast<const MSLLHOOKSTRUCT *>(lparam))) return 1;
        } catch (...) {
        }
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

LRESULT CALLBACK keyboard_proc(int code, WPARAM wparam, LPARAM lparam) noexcept {
    if (code >= 0 && current) {
        try {
            if (current->handle_key(static_cast<UINT>(wparam), *reinterpret_cast<const KBDLLHOOKSTRUCT *>(lparam))) return 1;
        } catch (...) {
        }
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

void CALLBACK foreground_proc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG, LONG, DWORD, DWORD) noexcept {
    // No messages are dispatched while the engine lock is held; this only guards future changes.
    if (event != EVENT_SYSTEM_FOREGROUND || in_engine || !current) return;
    try {
        current->with_engine([&](Engine &engine, Events &out) {
            engine.on_foreground(reinterpret_cast<intptr_t>(hwnd), out);
        });
    } catch (...) {
    }
}
}

InputHooks::InputHooks() {
    if (hooks_exist.exchange(true)) throw std::runtime_error("only one input hub may exist per process");
    try {
        state_ = std::make_unique<State>();
        std::promise<void> ready;
        auto started = ready.get_future();
        // The promise moves into the thread: set_value may still be finishing when wait returns.
        state_->thread = std::thread([state = state_.get(), ready = std::move(ready)]() mutable { state->run(ready); });
        started.wait();
    } catch (const std::exception &e) {
        state_.reset();
        hooks_exist.store(false);
        throw std::runtime_error(e.what());
    }
}

InputHooks::~InputHooks() { stop(); }

void InputHooks::with_engine(const std::function<void(Engine &, Events &)> &f) { state_->with_engine(f); }

std::optional<Event> InputHooks::next_event(int timeout_ms) {
    std::unique_lock<std::mutex> lock(state_->queue_mutex);
    const auto ready = [this] { return !state_->queue.empty() || state_->queue_closed; };
    if (timeout_ms < 0) state_->queue_ready.wait(lock, ready);
    else state_->queue_ready.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready);
    if (state_->queue.empty()) return std::nullopt;
    Event event = std::move(state_->queue.front());
    state_->queue.pop_front();
    return event;
}

void InputHooks::stop() {
    if (!state_) return;
    std::lock_guard<std::mutex> stop_lock(state_->stop_mutex);
    state_->with_engine([](Engine &engine, Events &out) { engine.close(out); });
    {
        std::lock_guard<std::mutex> queue_lock(state_->queue_mutex);
        state_->queue_closed = true;
    }
    state_->queue_ready.notify_all();
    if (state_->thread.joinable()) {
        if (const DWORD id = state_->thread_id.load(std::memory_order_acquire)) PostThreadMessageW(id, WM_QUIT, 0, 0);
        state_->thread.join();
        hooks_exist.store(false);
    }
}

void InputHooks::set_test_marker(uintptr_t marker) { state_->test_marker.store(marker, std::memory_order_release); }

bool InputHooks::hooks_installed() const { return state_->hooks_installed.load(std::memory_order_acquire); }
}
