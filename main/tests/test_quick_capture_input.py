"""Quick capture input without installing hooks or injecting real input."""

import ctypes
from types import SimpleNamespace

import pytest
from PySide6.QtCore import Qt

from core import quick_capture_input as input_module
from core.quick_capture_input import (
    QuickCaptureInput, WM_KEYDOWN, WM_KEYUP, WM_SYSKEYUP,
    WM_LBUTTONDOWN, WM_LBUTTONUP, WM_RBUTTONDOWN, WM_RBUTTONUP, WM_MBUTTONDOWN, WM_MBUTTONUP,
    WM_XBUTTONDOWN, WM_XBUTTONUP, WM_MOUSEMOVE, WM_MOUSEWHEEL, WM_MOUSEHWHEEL, VK_ESCAPE,
)

_RealWin32Hooks = input_module._Win32Hooks

# button: (down message, up message, mouseData high word)
BUTTONS = {
    "left": (WM_LBUTTONDOWN, WM_LBUTTONUP, 0),
    "middle": (WM_MBUTTONDOWN, WM_MBUTTONUP, 0),
    "right": (WM_RBUTTONDOWN, WM_RBUTTONUP, 0),
    "x1": (WM_XBUTTONDOWN, WM_XBUTTONUP, 1),
    "x2": (WM_XBUTTONDOWN, WM_XBUTTONUP, 2),
}


class FakeHooks:
    def __init__(self, mouse, keyboard, failure):
        self.mouse, self.keyboard, self.failure = mouse, keyboard, failure
        self.started = self.stopped = False

    def start(self):
        self.started = True

    def stop(self):
        self.stopped = True


@pytest.fixture
def capture_input(qapp, monkeypatch):
    created, down, masks, events, errors, moves = [], set(), [], [], [], []

    def backend(*callbacks):
        result = FakeHooks(*callbacks)
        created.append(result)
        return result

    def modifiers(exclude_vk=None):
        return frozenset(name for name, keys in input_module._MODIFIER_KEYS.items()
                         if any(vk in down and vk != exclude_vk for vk in keys))

    monkeypatch.setattr(input_module, "_Win32Hooks", backend)
    monkeypatch.setattr(input_module, "_current_modifiers", modifiers)
    monkeypatch.setattr(input_module, "_current_menu_keys", lambda modifiers: {
        vk for name in modifiers & {"win", "alt"}
        for vk in input_module._MODIFIER_KEYS[name] if vk in down
    })
    monkeypatch.setattr(input_module, "_mask_modifier_menu", lambda: masks.append(True))
    source = QuickCaptureInput()
    source.event.connect(lambda *args: events.append(args), Qt.ConnectionType.QueuedConnection)
    source.failure.connect(errors.append, Qt.ConnectionType.QueuedConnection)
    source.moved.connect(moves.append, Qt.ConnectionType.QueuedConnection)
    result = SimpleNamespace(source=source, created=created, down=down, masks=masks,
                             events=events, errors=errors, moves=moves, flush=qapp.processEvents)
    yield result
    source.close()
    qapp.processEvents()
    source.deleteLater()


def mouse(fixture, message, x=10, y=20, flags=0, backend=None, high=0):
    return (backend or fixture.created[-1]).mouse(
        message, SimpleNamespace(pt=SimpleNamespace(x=x, y=y), flags=flags,
                                 mouseData=(high & 0xFFFF) << 16))


def press(fixture, button, x=10, y=20, flags=0):
    down, _up, high = BUTTONS[button]
    return mouse(fixture, down, x, y, flags=flags, high=high)


def release(fixture, button, x=10, y=20, flags=0):
    _down, up, high = BUTTONS[button]
    return mouse(fixture, up, x, y, flags=flags, high=high)


def keyboard(fixture, message, key, flags=0, backend=None):
    return (backend or fixture.created[-1]).keyboard(
        message, SimpleNamespace(vkCode=key, flags=flags))


def binding(*modifiers, button="left"):
    return frozenset(modifiers), button


WIN = binding("win")
CTRL = binding("ctrl")


def start(fixture, gesture=WIN, keys=(0x5B,)):
    fixture.source.configure([gesture], True)
    fixture.down.update(keys)
    assert press(fixture, gesture[1])


def test_drag_queues_only_endpoints_and_preserves_negative_coordinates(capture_input):
    f = capture_input
    start(f)
    assert f.source.dragging
    assert f.events == []
    for x in range(-100, 0):
        assert not mouse(f, WM_MOUSEMOVE, x, -30)
    assert mouse(f, WM_LBUTTONUP, -1, -30)
    assert not f.source.dragging
    f.flush()
    assert f.events == [("start", 1, 10, 20), ("finish", 1, -1, -30)]
    assert f.source.accepts(1)
    assert not f.masks
    assert not keyboard(f, WM_KEYUP, 0x5B)
    assert f.masks == [True]
    assert not mouse(f, WM_LBUTTONUP)


def test_move_burst_queues_one_notification_and_consumes_latest_position(capture_input):
    f = capture_input
    start(f)
    for x in range(1000):
        assert not mouse(f, WM_MOUSEMOVE, x, -x)
    assert not f.moves  # notification must cross the GUI event queue
    f.flush()
    assert f.moves == [1]
    assert f.source.take_position(1) == (999, -999)
    assert f.source.take_position(1) is None
    assert not mouse(f, WM_MOUSEMOVE, 1200, -1100)
    f.flush()
    assert f.moves == [1, 1]
    assert f.source.take_position(1) == (1200, -1100)


def test_move_consumer_rearms_before_next_gui_delivery(capture_input):
    f = capture_input
    consumed = []
    f.source.moved.connect(lambda token: consumed.append(f.source.take_position(token)),
                           Qt.ConnectionType.QueuedConnection)
    start(f)
    assert not mouse(f, WM_MOUSEMOVE, -40, 80)
    f.flush()
    assert consumed == [(-40, 80)]
    assert not mouse(f, WM_MOUSEMOVE, -90, 120)
    f.flush()
    assert consumed == [(-40, 80), (-90, 120)]


def test_stale_move_cannot_consume_new_gesture_position(capture_input):
    f = capture_input
    start(f)
    assert not mouse(f, WM_MOUSEMOVE, 40, 60)
    f.source.cancel()
    assert mouse(f, WM_LBUTTONUP)
    assert mouse(f, WM_LBUTTONDOWN)
    assert not mouse(f, WM_MOUSEMOVE, 140, 160)
    f.flush()
    assert f.moves == [1, 2]
    assert f.source.take_position(1) is None
    assert f.source.take_position(2) == (140, 160)


@pytest.mark.parametrize("transition", ["cancel", "disable", "reconfigure", "close", "failure"])
def test_gesture_invalidation_discards_pending_motion(capture_input, transition):
    f = capture_input
    start(f)
    assert not mouse(f, WM_MOUSEMOVE, 70, 80)
    if transition == "cancel":
        f.source.cancel()
    elif transition == "disable":
        f.source.configure([WIN], False)
    elif transition == "reconfigure":
        f.source.configure([CTRL], True)
    elif transition == "close":
        f.source.close()
    else:
        f.created[-1].failure("hook failed")
    f.flush()
    assert f.moves == [1]
    assert f.source.take_position(1) is None


def test_reapplying_same_configuration_preserves_pending_motion(capture_input):
    f = capture_input
    start(f)
    assert not mouse(f, WM_MOUSEMOVE, 70, 80)
    f.source.configure([WIN], True)
    f.flush()
    assert f.moves == [1]
    assert f.source.take_position(1) == (70, 80)
    assert f.source.dragging


def test_finish_discards_queued_motion_and_uses_exact_release_coordinates(capture_input):
    f = capture_input
    start(f)
    assert not mouse(f, WM_MOUSEMOVE, 70, 80)
    assert mouse(f, WM_LBUTTONUP, 170, 180)
    # A later unclaimed move must not replace the completed selection endpoint.
    assert not mouse(f, WM_MOUSEMOVE, 500, 600)
    f.flush()
    assert f.moves == [1]
    assert f.source.take_position(1) is None
    assert f.events == [("start", 1, 10, 20), ("finish", 1, 170, 180)]


@pytest.mark.parametrize("flags", [1, 2, 3])
def test_injected_motion_does_not_change_position_or_notify(capture_input, flags):
    f = capture_input
    start(f)
    assert not mouse(f, WM_MOUSEMOVE, 900, 1000, flags=flags)
    f.flush()
    assert not f.moves
    assert f.source._position == (10, 20)
    assert f.source.take_position(1) is None


def test_motion_outside_gesture_does_not_notify(capture_input):
    f = capture_input
    f.source.configure([WIN], True)
    assert not mouse(f, WM_MOUSEMOVE, 70, 80)
    start(f)
    f.source.cancel()
    assert not mouse(f, WM_MOUSEMOVE, 90, 100)
    f.flush()
    assert not f.moves


def test_blocked_input_passes_matching_clicks_then_resumes_without_reinstall(capture_input):
    f = capture_input
    f.source.configure([CTRL], True)
    f.down.add(0xA2)
    f.source.set_blocked(True)
    assert not mouse(f, WM_LBUTTONDOWN)
    assert not mouse(f, WM_LBUTTONUP)
    f.flush()
    assert not f.events
    f.source.set_blocked(False)
    assert mouse(f, WM_LBUTTONDOWN)
    assert mouse(f, WM_LBUTTONUP)
    f.flush()
    assert [event[0] for event in f.events] == ["start", "finish"]
    assert len(f.created) == 1


def test_blocking_active_drag_cancels_but_drains_owned_input_pairs(capture_input):
    f = capture_input
    start(f)
    f.source.set_blocked(True)
    assert not f.source.dragging
    assert not f.source.accepts(1)
    assert mouse(f, WM_LBUTTONUP)
    assert not keyboard(f, WM_KEYUP, 0x5B)
    assert f.masks == [True]
    assert not mouse(f, WM_LBUTTONDOWN)
    assert not mouse(f, WM_LBUTTONUP)
    f.flush()
    assert [event[0] for event in f.events] == ["start", "cancel"]


@pytest.mark.parametrize("name,key", [(name, key) for name, keys in input_module._MODIFIER_KEYS.items()
                                       for key in keys])
def test_left_and_right_modifiers(capture_input, name, key):
    f = capture_input
    start(f, binding(name), (key,))
    assert mouse(f, WM_LBUTTONUP)
    assert not f.masks
    assert not keyboard(f, WM_KEYUP, key)
    assert bool(f.masks) == (name in {"win", "alt"})


def test_two_modifiers_require_exact_match(capture_input):
    f = capture_input
    f.source.configure([binding("ctrl", "shift")], True)
    f.down.add(0xA2)
    assert not mouse(f, WM_LBUTTONDOWN)
    f.down.update((0xA0, 0x5B))
    assert not mouse(f, WM_LBUTTONDOWN)
    f.down.remove(0x5B)
    assert mouse(f, WM_LBUTTONDOWN)
    assert mouse(f, WM_LBUTTONUP)


@pytest.mark.parametrize("gesture", [binding(), binding("unknown"), binding("ctrl", "alt", "shift"),
                                     binding("win", button="x3"), binding("win", button="")])
def test_invalid_bindings_cannot_capture_plain_clicks(capture_input, gesture):
    f = capture_input
    f.source.configure([gesture], True)
    f.flush()
    assert not f.created
    assert len(f.errors) == 1


def test_each_configured_modifier_set_starts_its_own_gesture(capture_input):
    f = capture_input
    f.source.configure([WIN, binding("shift", "win")], True)
    f.down.update({0x5B})
    assert mouse(f, WM_LBUTTONDOWN)
    assert f.source.gesture_binding(1) == WIN
    assert mouse(f, WM_LBUTTONUP)
    f.down.add(0xA0)
    assert mouse(f, WM_LBUTTONDOWN)
    assert f.source.gesture_binding(2) == binding("shift", "win")
    assert f.source.gesture_binding(1) is None
    assert mouse(f, WM_LBUTTONUP)
    f.down.clear()
    f.down.add(0xA2)
    assert not mouse(f, WM_LBUTTONDOWN)


def test_modifiers_must_match_one_set_exactly(capture_input):
    f = capture_input
    f.source.configure([binding("ctrl", "win")], True)
    f.down.update({0x5B})
    assert not mouse(f, WM_LBUTTONDOWN)
    f.down.update({0xA2, 0xA0})
    assert not mouse(f, WM_LBUTTONDOWN)
    f.down.discard(0xA0)
    assert mouse(f, WM_LBUTTONDOWN)


@pytest.mark.parametrize("button", BUTTONS)
def test_every_mouse_button_drags_with_its_own_pair(capture_input, button):
    f = capture_input
    start(f, binding("win", button=button))
    assert f.source.gesture_binding(1) == binding("win", button=button)
    assert not mouse(f, WM_MOUSEMOVE, 50, 60)
    # Releases of other buttons belong to the desktop, not to this gesture.
    for other in BUTTONS:
        if other != button:
            assert not release(f, other)
    assert f.source.dragging
    assert release(f, button, 70, 80)
    assert not f.source.dragging
    assert not release(f, button)
    f.flush()
    assert f.events == [("start", 1, 10, 20), ("finish", 1, 70, 80)]


@pytest.mark.parametrize("button", BUTTONS)
def test_unbound_buttons_pass_through_with_the_same_modifiers(capture_input, button):
    f = capture_input
    others = [other for other in BUTTONS if other != button]
    f.source.configure([binding("win", button=other) for other in others], True)
    f.down.add(0x5B)
    assert not press(f, button)
    assert not release(f, button)
    f.flush()
    assert not f.events


def test_side_buttons_are_told_apart_by_mouse_data(capture_input):
    f = capture_input
    f.source.configure([binding("win", button="x2")], True)
    f.down.add(0x5B)
    assert not press(f, "x1")
    assert not release(f, "x1")
    assert not mouse(f, WM_XBUTTONDOWN, high=3)  # unknown side button
    assert press(f, "x2")
    assert release(f, "x2")
    f.flush()
    assert [event[0] for event in f.events] == ["start", "finish"]


def test_same_modifiers_on_different_buttons_start_separate_gestures(capture_input):
    f = capture_input
    f.source.configure([WIN, binding("win", button="right")], True)
    f.down.add(0x5B)
    assert press(f, "right")
    assert f.source.gesture_binding(1) == binding("win", button="right")
    assert release(f, "right")
    assert press(f, "left")
    assert f.source.gesture_binding(2) == WIN
    assert release(f, "left")
    f.flush()
    assert [event[:2] for event in f.events] == [("start", 1), ("finish", 1), ("start", 2), ("finish", 2)]


def test_other_buttons_during_drag_are_swallowed_in_pairs(capture_input):
    f = capture_input
    f.source.configure([WIN, binding("win", button="right")], True)
    f.down.add(0x5B)
    assert press(f, "left")
    # A bound button cannot start a second gesture; an unbound one cannot reach the desktop.
    assert press(f, "right")
    assert press(f, "x1")
    assert f.source.gesture_binding(1) == WIN
    assert release(f, "right")
    assert f.source.dragging
    assert release(f, "left", 70, 80)
    assert release(f, "x1")  # pressed during the drag, released after it
    assert not release(f, "x1")
    f.flush()
    assert f.events == [("start", 1, 10, 20), ("finish", 1, 70, 80)]


def test_old_button_release_cannot_finish_a_new_gesture(capture_input):
    f = capture_input
    f.source.configure([WIN, binding("win", button="middle")], True)
    f.down.add(0x5B)
    assert press(f, "left")
    assert keyboard(f, WM_KEYDOWN, VK_ESCAPE)
    assert press(f, "middle")
    assert f.source.gesture_binding(2) == binding("win", button="middle")
    assert release(f, "left")
    assert f.source.dragging
    assert release(f, "middle", 70, 80)
    f.flush()
    assert [event[:2] for event in f.events] == [("start", 1), ("cancel", 1), ("start", 2), ("finish", 2)]


@pytest.mark.parametrize("message", [WM_MOUSEWHEEL, WM_MOUSEHWHEEL])
def test_wheels_are_swallowed_only_during_a_drag(capture_input, message):
    f = capture_input
    f.source.configure([WIN], True)
    assert not mouse(f, message, high=120)
    start(f)
    assert mouse(f, message, high=120)
    assert not mouse(f, message, high=120, flags=1)
    f.source.cancel()
    assert not mouse(f, message, high=120)


def test_disabled_configuration_is_idle_and_repeated_enable_is_idempotent(capture_input):
    f = capture_input
    f.source.configure([WIN], False)
    assert not f.created
    f.source.configure([WIN], True)
    f.source.configure([WIN], True)
    assert len(f.created) == 1
    assert not mouse(f, WM_MOUSEMOVE)
    assert not mouse(f, WM_LBUTTONDOWN)
    assert not keyboard(f, WM_KEYDOWN, VK_ESCAPE)


def test_escape_cancels_and_swallows_both_input_pairs(capture_input):
    f = capture_input
    start(f)
    assert keyboard(f, WM_KEYDOWN, VK_ESCAPE)
    assert keyboard(f, WM_KEYDOWN, VK_ESCAPE)  # repeat
    assert not f.source.dragging
    assert not f.source.accepts(1)
    assert not mouse(f, WM_MOUSEMOVE)
    assert mouse(f, WM_LBUTTONUP)
    assert keyboard(f, WM_KEYUP, VK_ESCAPE)
    assert not keyboard(f, WM_KEYUP, VK_ESCAPE)
    f.flush()
    assert [entry[0] for entry in f.events] == ["start", "cancel"]


def test_modifier_release_cancels_but_does_not_swallow_real_release(capture_input):
    f = capture_input
    start(f)
    # Async state still includes this key while the hook is called.
    assert not keyboard(f, WM_KEYUP, 0x5B)
    assert not f.source.dragging
    assert mouse(f, WM_LBUTTONUP)
    f.flush()
    assert [entry[0] for entry in f.events] == ["start", "cancel"]


def test_holding_other_side_of_modifier_keeps_drag_alive(capture_input):
    f = capture_input
    start(f, keys=(0x5B, 0x5C))
    assert not keyboard(f, WM_KEYUP, 0x5B)
    assert f.source.dragging
    f.down.remove(0x5B)
    assert not keyboard(f, WM_KEYUP, 0x5C)
    assert not f.source.dragging


def test_alt_system_key_release_is_cancelled_normally(capture_input):
    f = capture_input
    start(f, binding("alt"), (0xA5,))
    assert not keyboard(f, WM_SYSKEYUP, 0xA5)
    assert not f.source.dragging
    assert mouse(f, WM_LBUTTONUP)


@pytest.mark.parametrize("flags", [1, 2, 3])
@pytest.mark.parametrize("button", ["left", "x1"])
def test_injected_mouse_never_starts_or_finishes_a_physical_drag(capture_input, flags, button):
    f = capture_input
    f.source.configure([binding("win", button=button)], True)
    f.down.add(0x5B)
    assert not press(f, button, flags=flags)
    assert press(f, button)
    assert not release(f, button, flags=flags)
    assert f.source.dragging
    assert release(f, button)


@pytest.mark.parametrize("flags", [0x10, 0x02, 0x12])
def test_injected_keyboard_does_not_cancel(capture_input, flags):
    f = capture_input
    start(f)
    assert not keyboard(f, WM_KEYDOWN, VK_ESCAPE, flags)
    assert not keyboard(f, WM_KEYUP, 0x5B, flags)
    assert f.source.dragging


def test_disable_waits_for_claimed_buttons_and_escape_releases(capture_input):
    f = capture_input
    start(f, binding("win", button="right"))
    assert press(f, "middle")
    keyboard(f, WM_KEYDOWN, VK_ESCAPE)
    f.source.configure([WIN], False)
    assert not f.created[-1].stopped
    assert release(f, "right")
    f.flush()
    assert not f.created[-1].stopped
    assert release(f, "middle")
    f.flush()
    assert not f.created[-1].stopped
    assert keyboard(f, WM_KEYUP, VK_ESCAPE)
    f.flush()
    assert not f.created[-1].stopped
    assert not keyboard(f, WM_KEYUP, 0x5B)
    f.flush()
    assert f.created[-1].stopped


def test_reconfigure_invalidates_queued_finish_and_old_callbacks(capture_input):
    f = capture_input
    start(f)
    first = f.created[-1]
    assert mouse(f, WM_LBUTTONUP)
    f.source.configure([CTRL], True)
    assert not f.source.accepts(1)
    assert not keyboard(f, WM_KEYUP, 0x5B)
    f.down.clear()
    f.source.configure([CTRL], False)
    assert first.stopped
    f.source.configure([CTRL], True)
    assert len(f.created) == 2
    assert not mouse(f, WM_LBUTTONDOWN, backend=first)
    first.failure("stale")
    f.flush()
    assert not f.errors


def test_cancel_invalidates_finished_signal_and_next_gesture_is_distinct(capture_input):
    f = capture_input
    start(f)
    assert mouse(f, WM_LBUTTONUP)
    f.source.cancel()
    assert not f.source.accepts(1)
    assert mouse(f, WM_LBUTTONDOWN)
    assert mouse(f, WM_LBUTTONUP)
    assert f.source.accepts(2)
    assert not f.source.accepts(3)


def test_reconfigure_during_drag_keeps_mouse_pair_claimed(capture_input):
    f = capture_input
    start(f)
    f.source.configure([CTRL], True)
    assert not f.source.dragging
    assert mouse(f, WM_LBUTTONDOWN)  # no second start until the real UP
    assert mouse(f, WM_LBUTTONUP)
    f.flush()
    assert [entry[0] for entry in f.events] == ["start", "cancel"]


def test_close_is_final_and_stops_backend(capture_input):
    f = capture_input
    start(f)
    first = f.created[-1]
    f.source.close()
    f.source.close()
    assert first.stopped
    assert not f.source.accepts(1)
    assert not mouse(f, WM_LBUTTONDOWN, backend=first)
    f.source.configure([WIN], True)
    assert len(f.created) == 1


def test_hook_start_failure_and_async_failure_are_reported(capture_input, monkeypatch):
    f = capture_input
    monkeypatch.setattr(FakeHooks, "start", lambda self: (_ for _ in ()).throw(OSError("start failed")))
    f.source.configure([WIN], True)
    f.flush()
    assert f.errors == ["start failed"]
    assert f.created[-1].stopped


def test_async_hook_failure_cancels_and_invalidates_gesture(capture_input):
    f = capture_input
    start(f)
    f.created[-1].failure("hook failed")
    f.flush()
    assert f.errors == ["hook failed"]
    assert not f.source.dragging
    assert not f.source.accepts(1)
    assert f.created[-1].stopped


def test_mask_failure_does_not_swallow_real_modifier_release(capture_input, monkeypatch):
    f = capture_input
    monkeypatch.setattr(input_module, "_mask_modifier_menu",
                        lambda: (_ for _ in ()).throw(OSError("mask failed")))
    start(f)
    assert f.source.dragging
    assert mouse(f, WM_LBUTTONUP)
    assert not keyboard(f, WM_KEYUP, 0x5B)
    f.flush()
    assert f.errors == ["mask failed"]
    assert [entry[0] for entry in f.events] == ["start", "finish"]
    assert not f.source._pending_menu_keys
    assert not keyboard(f, WM_KEYUP, 0x5B)
    f.flush()
    assert f.errors == ["mask failed"]


@pytest.mark.parametrize("key", [0x5B, 0x5C])
def test_long_win_drag_masks_only_final_release_even_after_mouse_up(capture_input, key):
    f = capture_input
    start(f, keys=(key,))
    for _ in range(20):
        assert not keyboard(f, WM_KEYDOWN, key)
    assert mouse(f, WM_LBUTTONUP)
    f.flush()
    # Capture may already have created a pin and changed foreground focus;
    # Win autorepeat must not exhaust the pending release protection.
    for _ in range(20):
        assert not keyboard(f, WM_KEYDOWN, key)
    assert not f.masks
    assert not keyboard(f, WM_KEYUP, key)
    assert f.masks == [True]
    f.flush()
    assert [entry[0] for entry in f.events] == ["start", "finish"]
    assert not keyboard(f, WM_KEYUP, key)
    assert f.masks == [True]


def test_both_win_keys_keep_independent_release_protection(capture_input):
    f = capture_input
    start(f, keys=(0x5B, 0x5C))
    assert mouse(f, WM_LBUTTONUP)
    assert not keyboard(f, WM_KEYUP, 0x5B)
    f.down.remove(0x5B)
    assert f.masks == [True]
    assert not keyboard(f, WM_KEYDOWN, 0x5C)
    assert not keyboard(f, WM_KEYUP, 0x5C)
    assert f.masks == [True, True]
    assert not f.source._pending_menu_keys


def test_win_side_pressed_during_drag_keeps_protection_until_its_release(capture_input):
    f = capture_input
    start(f)
    assert not keyboard(f, WM_KEYDOWN, 0x5C)
    f.down.add(0x5C)
    assert not keyboard(f, WM_KEYUP, 0x5B)
    f.down.remove(0x5B)
    assert f.source.dragging
    assert f.masks == [True]
    assert mouse(f, WM_LBUTTONUP)
    assert not keyboard(f, WM_KEYDOWN, 0x5C)
    assert not keyboard(f, WM_KEYUP, 0x5C)
    assert f.masks == [True, True]
    assert not f.source._pending_menu_keys
    f.flush()
    assert [entry[0] for entry in f.events] == ["start", "finish"]


@pytest.mark.parametrize("key", [0xA4, 0xA5])
def test_completed_alt_drag_masks_system_key_release(capture_input, key):
    f = capture_input
    start(f, binding("alt"), (key,))
    assert mouse(f, WM_LBUTTONUP)
    assert not f.masks
    assert not keyboard(f, WM_SYSKEYUP, key)
    assert f.masks == [True]


@pytest.mark.parametrize("transition", ["cancel", "disable", "reconfigure"])
def test_menu_release_protection_survives_gesture_state_changes(capture_input, transition):
    f = capture_input
    start(f)
    if transition == "cancel":
        f.source.cancel()
    elif transition == "disable":
        f.source.configure([WIN], False)
    else:
        f.source.configure([CTRL], True)
    assert not f.source.dragging
    assert mouse(f, WM_LBUTTONUP)
    f.flush()
    assert not f.created[-1].stopped
    assert not keyboard(f, WM_KEYDOWN, 0x5B)  # repeat after cancellation
    assert not f.masks
    assert not keyboard(f, WM_KEYUP, 0x5B)
    f.flush()
    assert f.masks == [True]
    assert f.created[-1].stopped == (transition == "disable")


def test_disabling_after_completed_capture_waits_for_win_release(capture_input):
    f = capture_input
    start(f)
    assert mouse(f, WM_LBUTTONUP)
    f.flush()
    f.source.configure([WIN], False)
    assert not f.created[-1].stopped
    assert not keyboard(f, WM_KEYUP, 0x5B)
    f.flush()
    assert f.masks == [True]
    assert f.created[-1].stopped


@pytest.mark.parametrize("key", [0x5B, 0x5C])
def test_plain_win_key_before_and_after_capture_keeps_native_behavior(capture_input, key):
    f = capture_input
    f.source.configure([WIN], True)
    assert not keyboard(f, WM_KEYDOWN, key)
    assert not keyboard(f, WM_KEYUP, key)
    assert not f.masks
    start(f, keys=(key,))
    assert mouse(f, WM_LBUTTONUP)
    assert not keyboard(f, WM_KEYUP, key)
    f.down.remove(key)
    assert f.masks == [True]
    assert not keyboard(f, WM_KEYDOWN, key)
    assert not keyboard(f, WM_KEYUP, key)
    assert f.masks == [True]


@pytest.mark.parametrize("flags", [0x10, 0x02, 0x12])
def test_injected_win_release_does_not_consume_physical_release_protection(capture_input, flags):
    f = capture_input
    start(f)
    assert mouse(f, WM_LBUTTONUP)
    assert not keyboard(f, WM_KEYUP, 0x5B, flags)
    assert not f.masks
    assert not keyboard(f, WM_KEYDOWN, 0x5B)
    assert not keyboard(f, WM_KEYUP, 0x5B)
    assert f.masks == [True]


def test_unrelated_menu_key_release_is_not_masked(capture_input):
    f = capture_input
    start(f)
    assert mouse(f, WM_LBUTTONUP)
    assert not keyboard(f, WM_KEYUP, 0x5C)
    assert not keyboard(f, WM_SYSKEYUP, 0xA4)
    assert not f.masks
    assert not keyboard(f, WM_KEYUP, 0x5B)
    assert f.masks == [True]


def test_other_keys_during_drag_reach_the_desktop(capture_input):
    f = capture_input
    start(f)
    for key in (0x43, 0xBB, 0xA1):
        assert not keyboard(f, WM_KEYDOWN, key)
        assert not keyboard(f, WM_KEYUP, key)
    assert f.source.dragging


class FakeFunction:
    def __init__(self, function):
        self.function = function

    def __call__(self, *args):
        return self.function(*args)


class FakeNativeApi:
    """Every Win32 entry point used by the backend is intercepted."""

    def __init__(self, install_results=(101, 102), messages=(0,)):
        self.installed, self.unhooked, self.posted = [], [], []
        results, messages = iter(install_results), iter(messages)

        def install(kind, callback, module, thread):
            self.installed.append((kind, callback, module, thread))
            return next(results)

        self.SetWindowsHookExW = FakeFunction(install)
        self.UnhookWindowsHookEx = FakeFunction(lambda hook: self.unhooked.append(hook) or 1)
        self.CallNextHookEx = FakeFunction(lambda *_args: 123)
        self.PeekMessageW = FakeFunction(lambda *_args: 0)
        self.GetMessageW = FakeFunction(lambda *_args: next(messages))
        self.PostThreadMessageW = FakeFunction(lambda *args: self.posted.append(args) or 1)
        self.GetCurrentThreadId = FakeFunction(lambda: 321)


@pytest.fixture
def native_api(monkeypatch):
    api = FakeNativeApi()
    monkeypatch.setattr(ctypes, "WinDLL", lambda *_args, **_kwargs: api)
    return api


def test_native_backend_installs_both_hooks_and_unhooks_in_reverse(native_api):
    errors = []
    backend = input_module._Win32Hooks(lambda *_: False, lambda *_: False, errors.append)
    backend.run()
    assert [entry[0] for entry in native_api.installed] == [14, 13]
    assert native_api.unhooked == [102, 101]
    assert not errors
    assert backend._thread_id is None
    assert native_api.CallNextHookEx.restype is ctypes.c_ssize_t


def test_native_partial_install_failure_releases_first_hook(monkeypatch):
    api = FakeNativeApi(install_results=(101, None))
    monkeypatch.setattr(ctypes, "WinDLL", lambda *_args, **_kwargs: api)
    errors = []
    backend = input_module._Win32Hooks(lambda *_: False, lambda *_: False, errors.append)
    backend.run()
    assert api.unhooked == [101]
    assert len(errors) == 1


def test_native_message_failure_releases_both_hooks(monkeypatch):
    api = FakeNativeApi(messages=(-1,))
    monkeypatch.setattr(ctypes, "WinDLL", lambda *_args, **_kwargs: api)
    errors = []
    input_module._Win32Hooks(lambda *_: False, lambda *_: False, errors.append).run()
    assert api.unhooked == [102, 101]
    assert len(errors) == 1


def test_native_stop_wakes_message_loop_and_can_precede_start(native_api):
    backend = input_module._Win32Hooks(lambda *_: False, lambda *_: False, lambda *_: None)
    backend.stop()
    backend.run()
    assert not native_api.installed
    backend._thread_id = 321
    backend.stop()
    assert native_api.posted == [(321, 0x0012, 0, 0)]


def test_native_stop_keeps_thread_id_when_worker_finishes_concurrently(monkeypatch):
    api = FakeNativeApi()
    backend = input_module._Win32Hooks(lambda *_: False, lambda *_: False, lambda *_: None)
    backend._thread_id = 321

    def load_library(*_args, **_kwargs):
        backend._thread_id = None  # worker's finally runs during the Win32 call setup
        return api

    monkeypatch.setattr(ctypes, "WinDLL", load_library)
    backend.stop()
    assert api.posted == [(321, 0x0012, 0, 0)]


def test_native_callback_suppresses_claims_and_passes_other_events(native_api):
    backend = input_module._Win32Hooks(lambda *_: True, lambda *_: False, lambda *_: None)
    backend.run()
    mouse_callback = native_api.installed[0][1]
    data = input_module._MouseData()
    address = ctypes.addressof(data)
    assert mouse_callback(0, WM_LBUTTONDOWN, address) == 1
    assert mouse_callback(-1, WM_LBUTTONDOWN, address) == 123
    keyboard_callback = native_api.installed[1][1]
    assert keyboard_callback(0, WM_KEYDOWN, ctypes.addressof(input_module._KeyboardData())) == 123


def test_native_side_button_is_read_from_the_high_word_of_mouse_data(native_api):
    seen = []
    backend = input_module._Win32Hooks(
        lambda message, data: seen.append(input_module._button_of(input_module._BUTTON_DOWN, message, data)),
        lambda *_: False, lambda *_: None)
    backend.run()
    callback = native_api.installed[0][1]
    for high in (1, 2):
        data = input_module._MouseData(mouseData=high << 16)
        callback(0, WM_XBUTTONDOWN, ctypes.addressof(data))
    assert seen == ["x1", "x2"]


def test_menu_mask_precedes_forwarding_native_win_release(capture_input, native_api, monkeypatch):
    f = capture_input
    start(f)
    assert mouse(f, WM_LBUTTONUP)
    order = []
    monkeypatch.setattr(input_module, "_mask_modifier_menu", lambda: order.append("mask"))
    native_api.CallNextHookEx.function = lambda *_: order.append("release") or 123
    # The fixture replaces _Win32Hooks; use the saved real class to run
    # its ctypes callback against fake Win32 entry points, never the desktop.
    backend = _RealWin32Hooks(lambda *_: False, f.created[-1].keyboard, f.errors.append)
    backend.run()
    callback = native_api.installed[1][1]
    data = input_module._KeyboardData(vkCode=0x5B)
    assert callback(0, WM_KEYUP, ctypes.addressof(data)) == 123
    assert order == ["mask", "release"]


def test_native_callback_error_is_reported_and_stops_listener(native_api):
    errors = []

    def failure(*_args):
        raise ValueError("bad event")

    backend = input_module._Win32Hooks(failure, lambda *_: False, errors.append)
    backend.run()
    callback = native_api.installed[0][1]
    assert callback(0, WM_LBUTTONDOWN, ctypes.addressof(input_module._MouseData())) == 123
    assert errors == ["bad event"]
    assert backend._stopping.is_set()


def test_menu_mask_sends_paired_unassigned_key_and_checks_failure(monkeypatch):
    calls = []

    def send(count, inputs, size):
        calls.append((count, [(i.type, i.value.ki.wVk, i.value.ki.dwFlags) for i in inputs], size))
        return 2

    api = SimpleNamespace(SendInput=FakeFunction(send), GetAsyncKeyState=FakeFunction(lambda vk: 0))
    monkeypatch.setattr(ctypes, "WinDLL", lambda *_args, **_kwargs: api)
    input_module._user32.cache_clear()
    try:
        input_module._mask_modifier_menu()
        assert calls == [(2, [(1, 0xE8, 0), (1, 0xE8, 2)], ctypes.sizeof(input_module._Input))]
        assert ctypes.sizeof(input_module._Input) == (40 if ctypes.sizeof(ctypes.c_void_p) == 8 else 28)
        api.SendInput.function = lambda *_args: 0
        with pytest.raises(OSError):
            input_module._mask_modifier_menu()
    finally:
        input_module._user32.cache_clear()


def test_native_modifier_read_excludes_current_keyup(monkeypatch):
    down = {0xA2, 0xA0}
    api = SimpleNamespace(GetAsyncKeyState=FakeFunction(lambda vk: 0x8000 if vk in down else 0),
                          SendInput=FakeFunction(lambda *_args: 2))
    monkeypatch.setattr(ctypes, "WinDLL", lambda *_args, **_kwargs: api)
    input_module._user32.cache_clear()
    try:
        assert input_module._current_modifiers() == frozenset({"ctrl", "shift"})
        assert input_module._current_modifiers(exclude_vk=0xA2) == frozenset({"shift"})
    finally:
        input_module._user32.cache_clear()


def test_native_menu_key_read_keeps_actual_sides_and_filters_configuration(monkeypatch):
    down = {0x5B, 0x5C, 0xA5, 0xA0, 0xA2}
    api = SimpleNamespace(GetAsyncKeyState=FakeFunction(lambda vk: 0x8000 if vk in down else 0),
                          SendInput=FakeFunction(lambda *_args: 2))
    monkeypatch.setattr(ctypes, "WinDLL", lambda *_args, **_kwargs: api)
    input_module._user32.cache_clear()
    try:
        assert input_module._current_menu_keys(frozenset({"win"})) == {0x5B, 0x5C}
        assert input_module._current_menu_keys(frozenset({"win", "alt"})) == {0x5B, 0x5C, 0xA5}
        assert input_module._current_menu_keys(frozenset({"ctrl", "alt"})) == {0xA5}
        assert input_module._current_menu_keys(frozenset({"ctrl", "shift"})) == set()
    finally:
        input_module._user32.cache_clear()
