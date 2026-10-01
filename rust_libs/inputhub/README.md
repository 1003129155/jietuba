# j-input

Windows global input for Python, implemented in Rust.

One native thread owns the low-level mouse and keyboard hooks and a foreground-window event hook.
Every hook callback runs a plain state machine and decides on the spot whether to swallow the
event; it never calls into Python and never waits for the GIL. A busy Python main thread therefore
cannot stall the system's mouse or keyboard. Only the few events the application cares about are
queued for Python.

```python
import inputhub

hub = inputhub.Hub()                                   # one per process
hub.configure_gestures([(["ctrl"], "left"), (["win"], "right")], True)
hub.configure_side_buttons(True, ["x1"])               # swallow Back, report both side buttons
hub.bind_hotkey("clipboard", ["win"], 0x56)            # take Win+V from the system
hub.watch_wheel("stitch", (0, 0, 1920, 1080))          # wheel events inside this rectangle only
hub.watch_keys("stitch", [0x10, 0xA0, 0xA1])           # Shift
hub.watch_foreground("clipboard")                      # every foreground change, including this process

event = hub.next_event()           # blocks with the GIL released; next_event(100) waits at most 100 ms
# ("gesture", "start", id, x, y) / ("moved", id) / ("side", "x1") / ("hotkey", "clipboard") /
# ("wheel", watcher, x, y, delta, horizontal) / ("key", watcher, vk, pressed) /
# ("foreground", hwnd) / ("failure", message)

hub.take_position(gesture_id)      # latest cursor position of a drag, consumed once per ("moved", id)
hub.close()                        # a pending next_event() returns None once queued events are drained
```

## Gestures

A gesture binds one or two modifiers to a mouse button. It starts when the button is pressed while
exactly those modifiers are held, and finishes when the button is released. The press and release
are swallowed as a pair, so the window below never starts a drag. While a gesture is active, wheel
events and other clicks are swallowed; Escape or releasing one of its modifiers cancels it. Releasing
Win or Alt after a gesture sends a paired unassigned key first, so the Start menu or menu bar does not
open. `accepts(id)` tells whether a queued start or finish is still current after a cancel or a
reconfiguration.

A matching gesture takes precedence over a side-button hotkey on the same button.

Injected input is ignored by gestures. `set_test_marker(value)` makes injected input whose
`dwExtraInfo` equals `value` count as real, so tests can drive the real hooks.

## Hotkeys

A hotkey binds modifiers to a non-modifier key, and fires when that key is pressed while exactly those
modifiers are held. Unlike `RegisterHotKey`, it can take combinations the system already owns, such as
Win+V. The key's press, auto-repeats and release are swallowed together, and only the press is reported.
A key already held before the modifiers is left alone. Releasing Win or Alt afterwards is masked the same
way as after a gesture. Injected input is ignored unless it carries the test marker. `unbind_hotkey(name)`
gives the combination back; a release still owed for a swallowed press is swallowed as well.

## Hook lifetime

The low-level hooks are installed only while something needs them: enabled gestures or side buttons,
a hotkey, a wheel or key watcher, or a swallowed press still waiting for its release. The out-of-context
foreground-window hook is installed only while a foreground watcher exists. Foreground events are not
filtered; the caller decides which windows count.

`inputhub.Engine` drives the same state machine without installing hooks, with held keys supplied by
the caller; it exists for tests and may be called from any thread.

The distribution is `j-input`; the module imports as `inputhub`. Without the default `python`
feature the crate is a plain Rust library.
