#pragma once
#include "core/input_engine.h"
#include <functional>
#include <memory>
#include <optional>

namespace jietuba::input {
// The Win32 side of the input state machine: a native thread with the low-level mouse and
// keyboard hooks and the foreground window event hook. Hook callbacks only turn system
// structures into engine input and answer swallow or not on the spot; events for the UI go
// into a queue. The hooks install only while the engine needs them, so no system input event
// takes the detour otherwise.
class InputHooks {
public:
    // Starts the hook thread; throws std::runtime_error when one already exists in this process.
    InputHooks();
    ~InputHooks();
    InputHooks(const InputHooks &) = delete;
    InputHooks &operator=(const InputHooks &) = delete;

    // Runs f on the locked engine and queues the events it emits.
    void with_engine(const std::function<void(Engine &, Events &)> &f);
    // Waits for the next event, without limit when timeout_ms < 0; nullopt on timeout or once
    // stopped and the queued events are drained.
    std::optional<Event> next_event(int timeout_ms = -1);
    // Closes the engine, ends the event stream, removes the hooks and joins the thread.
    void stop();
    // 0 turns it off. Otherwise simulated input whose extra info equals the marker counts as
    // real input, so automated tests can drive the hooks.
    void set_test_marker(uintptr_t marker);
    bool hooks_installed() const;

    struct State;

private:
    std::unique_ptr<State> state_;
};
}
