"""Quick capture input on the real native state machine, without installing hooks or injecting input."""

from types import SimpleNamespace

import pytest
from PySide6.QtCore import Qt

from core.quick_capture_input import QuickCaptureInput
from tests.engine_input_hub import engine_input_hub

VK_ESCAPE = 0x1B
MODIFIER_KEYS = {
    "ctrl": (0xA2, 0xA3),
    "shift": (0xA0, 0xA1),
    "alt": (0xA4, 0xA5),
    "win": (0x5B, 0x5C),
}
BUTTONS = ("left", "middle", "right", "x1", "x2")


@pytest.fixture
def capture_input(qapp):
    hub = engine_input_hub()
    events, errors, moves = [], [], []
    source = QuickCaptureInput(hub=hub)
    source.event.connect(lambda *args: events.append(args), Qt.ConnectionType.QueuedConnection)
    source.failure.connect(errors.append, Qt.ConnectionType.QueuedConnection)
    source.moved.connect(moves.append, Qt.ConnectionType.QueuedConnection)
    yield SimpleNamespace(source=source, hub=hub, native=hub.native, events=events, errors=errors,
                          moves=moves, flush=qapp.processEvents)
    source.close()
    hub.close()
    qapp.processEvents()
    source.deleteLater()
    hub.deleteLater()


def move(fixture, x=10, y=20, injected=False):
    return fixture.native.mouse("move", x, y, injected=injected)


def press(fixture, button="left", x=10, y=20, injected=False):
    return fixture.native.mouse("down", x, y, button, injected=injected)


def release(fixture, button="left", x=10, y=20, injected=False):
    return fixture.native.mouse("up", x, y, button, injected=injected)


def key_down(fixture, key, injected=False):
    return fixture.native.key(key, True, injected)


def key_up(fixture, key, injected=False):
    return fixture.native.key(key, False, injected)


def binding(*modifiers, button="left"):
    return frozenset(modifiers), button


WIN = binding("win")
CTRL = binding("ctrl")


def start(fixture, gesture=WIN, keys=(0x5B,)):
    fixture.source.configure([gesture], True)
    fixture.native.hold(*keys)
    assert press(fixture, gesture[1])


def kinds(fixture):
    return [event[0] for event in fixture.events]


def test_drag_queues_only_endpoints_and_preserves_negative_coordinates(capture_input):
    f = capture_input
    start(f)
    assert f.source.dragging
    assert f.events == []
    for x in range(-100, 0):
        assert not move(f, x, -30)
    assert release(f, "left", -1, -30)
    assert not f.source.dragging
    f.flush()
    assert f.events == [("start", 1, 10, 20), ("finish", 1, -1, -30)]
    assert f.source.accepts(1)
    assert f.native.mask_calls == 0
    assert not key_up(f, 0x5B)
    assert f.native.mask_calls == 1
    assert not release(f)


def test_move_burst_queues_one_notification_and_consumes_latest_position(capture_input):
    f = capture_input
    start(f)
    for x in range(1000):
        assert not move(f, x, -x)
    assert not f.moves  # notification must cross the GUI event queue
    f.flush()
    assert f.moves == [1]
    assert f.source.take_position(1) == (999, -999)
    assert f.source.take_position(1) is None
    assert not move(f, 1200, -1100)
    f.flush()
    assert f.moves == [1, 1]
    assert f.source.take_position(1) == (1200, -1100)


def test_move_consumer_rearms_before_next_gui_delivery(capture_input):
    f = capture_input
    consumed = []
    f.source.moved.connect(lambda token: consumed.append(f.source.take_position(token)),
                           Qt.ConnectionType.QueuedConnection)
    start(f)
    assert not move(f, -40, 80)
    f.flush()
    assert consumed == [(-40, 80)]
    assert not move(f, -90, 120)
    f.flush()
    assert consumed == [(-40, 80), (-90, 120)]


def test_stale_move_cannot_consume_new_gesture_position(capture_input):
    f = capture_input
    start(f)
    assert not move(f, 40, 60)
    f.source.cancel()
    assert release(f)
    assert press(f)
    assert not move(f, 140, 160)
    f.flush()
    assert f.moves == [1, 2]
    assert f.source.take_position(1) is None
    assert f.source.take_position(2) == (140, 160)


@pytest.mark.parametrize("transition", ["cancel", "disable", "reconfigure", "close", "failure"])
def test_gesture_invalidation_discards_pending_motion(capture_input, transition):
    f = capture_input
    start(f)
    assert not move(f, 70, 80)
    if transition == "cancel":
        f.source.cancel()
    elif transition == "disable":
        f.source.configure([WIN], False)
    elif transition == "reconfigure":
        f.source.configure([CTRL], True)
    elif transition == "close":
        f.source.close()
    else:
        f.native.fail("hook failed")
    f.flush()
    assert f.moves == [1]
    assert f.source.take_position(1) is None


def test_reapplying_same_configuration_preserves_pending_motion(capture_input):
    f = capture_input
    start(f)
    assert not move(f, 70, 80)
    f.source.configure([WIN], True)
    f.flush()
    assert f.moves == [1]
    assert f.source.take_position(1) == (70, 80)
    assert f.source.dragging


def test_finish_discards_queued_motion_and_uses_exact_release_coordinates(capture_input):
    f = capture_input
    start(f)
    assert not move(f, 70, 80)
    assert release(f, "left", 170, 180)
    # A later unclaimed move must not replace the completed selection endpoint.
    assert not move(f, 500, 600)
    f.flush()
    assert f.moves == [1]
    assert f.source.take_position(1) is None
    assert f.events == [("start", 1, 10, 20), ("finish", 1, 170, 180)]


def test_injected_motion_does_not_change_position_or_notify(capture_input):
    f = capture_input
    start(f)
    assert not move(f, 900, 1000, injected=True)
    f.flush()
    assert not f.moves
    assert f.source.take_position(1) is None
    assert release(f, "left", 30, 40)
    f.flush()
    assert f.events[-1] == ("finish", 1, 30, 40)


def test_motion_outside_gesture_does_not_notify(capture_input):
    f = capture_input
    f.source.configure([WIN], True)
    assert not move(f, 70, 80)
    start(f)
    f.source.cancel()
    assert not move(f, 90, 100)
    f.flush()
    assert not f.moves


def test_blocked_input_passes_matching_clicks_then_resumes_without_reinstall(capture_input):
    f = capture_input
    f.source.configure([CTRL], True)
    f.native.hold(0xA2)
    f.source.set_blocked(True)
    assert f.native.hooks_needed
    assert not press(f)
    assert not release(f)
    f.flush()
    assert not f.events
    f.source.set_blocked(False)
    assert f.native.hooks_needed
    assert press(f)
    assert release(f)
    f.flush()
    assert kinds(f) == ["start", "finish"]


def test_blocking_active_drag_cancels_but_drains_owned_input_pairs(capture_input):
    f = capture_input
    start(f)
    f.source.set_blocked(True)
    assert not f.source.dragging
    assert not f.source.accepts(1)
    assert release(f)
    assert not key_up(f, 0x5B)
    assert f.native.mask_calls == 1
    f.native.hold(0x5B)
    assert not press(f)
    assert not release(f)
    f.flush()
    assert kinds(f) == ["start", "cancel"]


@pytest.mark.parametrize("name,key", [(name, key) for name, keys in MODIFIER_KEYS.items() for key in keys])
def test_left_and_right_modifiers(capture_input, name, key):
    f = capture_input
    start(f, binding(name), (key,))
    assert release(f)
    assert f.native.mask_calls == 0
    assert not key_up(f, key)
    assert f.native.mask_calls == (1 if name in {"win", "alt"} else 0)


def test_two_modifiers_require_exact_match(capture_input):
    f = capture_input
    f.source.configure([binding("ctrl", "shift")], True)
    f.native.hold(0xA2)
    assert not press(f)
    f.native.hold(0xA0, 0x5B)
    assert not press(f)
    f.native.release_all()
    f.native.hold(0xA2, 0xA0)
    assert press(f)
    assert release(f)


@pytest.mark.parametrize("gesture", [binding(), binding("unknown"), binding("ctrl", "alt", "shift"),
                                     binding("win", button="x3"), binding("win", button="")])
def test_invalid_bindings_cannot_capture_plain_clicks(capture_input, gesture):
    f = capture_input
    f.source.configure([gesture], True)
    f.flush()
    assert not f.native.hooks_needed
    assert len(f.errors) == 1
    f.native.hold(0x5B, 0xA2, 0xA4, 0xA0)
    assert not press(f)


def test_disabling_invalid_bindings_is_silent(capture_input):
    f = capture_input
    f.source.configure([binding("unknown")], False)
    f.source.configure(frozenset(), False)
    f.flush()
    assert not f.errors


def test_each_configured_modifier_set_starts_its_own_gesture(capture_input):
    f = capture_input
    f.source.configure([WIN, binding("shift", "win")], True)
    f.native.hold(0x5B)
    assert press(f)
    assert f.source.gesture_binding(1) == WIN
    assert release(f)
    f.native.hold(0xA0)
    assert press(f)
    assert f.source.gesture_binding(2) == binding("shift", "win")
    assert f.source.gesture_binding(1) is None
    assert release(f)
    f.native.release_all()
    f.native.hold(0xA2)
    assert not press(f)


def test_modifiers_must_match_one_set_exactly(capture_input):
    f = capture_input
    f.source.configure([binding("ctrl", "win")], True)
    f.native.hold(0x5B)
    assert not press(f)
    f.native.hold(0xA2, 0xA0)
    assert not press(f)
    f.native.release_all()
    f.native.hold(0x5B, 0xA2)
    assert press(f)


@pytest.mark.parametrize("button", BUTTONS)
def test_every_mouse_button_drags_with_its_own_pair(capture_input, button):
    f = capture_input
    start(f, binding("win", button=button))
    assert f.source.gesture_binding(1) == binding("win", button=button)
    assert not move(f, 50, 60)
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
    f.native.hold(0x5B)
    assert not press(f, button)
    assert not release(f, button)
    f.flush()
    assert not f.events


def test_side_buttons_are_distinct(capture_input):
    f = capture_input
    f.source.configure([binding("win", button="x2")], True)
    f.native.hold(0x5B)
    assert not press(f, "x1")
    assert not release(f, "x1")
    assert press(f, "x2")
    assert release(f, "x2")
    f.flush()
    assert kinds(f) == ["start", "finish"]


def test_same_modifiers_on_different_buttons_start_separate_gestures(capture_input):
    f = capture_input
    f.source.configure([WIN, binding("win", button="right")], True)
    f.native.hold(0x5B)
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
    f.native.hold(0x5B)
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
    f.native.hold(0x5B)
    assert press(f, "left")
    assert key_down(f, VK_ESCAPE)
    assert press(f, "middle")
    assert f.source.gesture_binding(2) == binding("win", button="middle")
    assert release(f, "left")
    assert f.source.dragging
    assert release(f, "middle", 70, 80)
    f.flush()
    assert [event[:2] for event in f.events] == [("start", 1), ("cancel", 1), ("start", 2), ("finish", 2)]


@pytest.mark.parametrize("action", ["wheel", "hwheel"])
def test_wheels_are_swallowed_only_during_a_drag(capture_input, action):
    f = capture_input
    f.source.configure([WIN], True)
    assert not f.native.mouse(action, delta=120)
    start(f)
    assert f.native.mouse(action, delta=120)
    assert not f.native.mouse(action, delta=120, injected=True)
    f.source.cancel()
    assert not f.native.mouse(action, delta=120)


def test_disabled_configuration_is_idle_and_repeated_enable_is_idempotent(capture_input):
    f = capture_input
    f.source.configure([WIN], False)
    assert not f.native.hooks_needed
    f.source.configure([WIN], True)
    f.source.configure([WIN], True)
    assert f.native.hooks_needed
    assert not move(f)
    assert not press(f)
    assert not key_down(f, VK_ESCAPE)


def test_escape_cancels_and_swallows_both_input_pairs(capture_input):
    f = capture_input
    start(f)
    assert key_down(f, VK_ESCAPE)
    assert key_down(f, VK_ESCAPE)  # repeat
    assert not f.source.dragging
    assert not f.source.accepts(1)
    assert not move(f)
    assert release(f)
    assert key_up(f, VK_ESCAPE)
    assert not key_up(f, VK_ESCAPE)
    f.flush()
    assert kinds(f) == ["start", "cancel"]


def test_modifier_release_cancels_but_does_not_swallow_real_release(capture_input):
    f = capture_input
    start(f)
    # Async state still includes this key while the hook is called.
    assert not key_up(f, 0x5B)
    assert not f.source.dragging
    assert release(f)
    f.flush()
    assert kinds(f) == ["start", "cancel"]


def test_holding_other_side_of_modifier_keeps_drag_alive(capture_input):
    f = capture_input
    start(f, keys=(0x5B, 0x5C))
    assert not key_up(f, 0x5B)
    assert f.source.dragging
    assert not key_up(f, 0x5C)
    assert not f.source.dragging


def test_alt_release_cancels_normally(capture_input):
    f = capture_input
    start(f, binding("alt"), (0xA5,))
    assert not key_up(f, 0xA5)
    assert not f.source.dragging
    assert release(f)


@pytest.mark.parametrize("button", ["left", "x1"])
def test_injected_mouse_never_starts_or_finishes_a_physical_drag(capture_input, button):
    f = capture_input
    f.source.configure([binding("win", button=button)], True)
    f.native.hold(0x5B)
    assert not press(f, button, injected=True)
    assert press(f, button)
    assert not release(f, button, injected=True)
    assert f.source.dragging
    assert release(f, button)


def test_injected_keyboard_does_not_cancel(capture_input):
    f = capture_input
    start(f)
    assert not key_down(f, VK_ESCAPE, injected=True)
    assert not key_up(f, 0x5B, injected=True)
    assert f.source.dragging


def test_disable_waits_for_claimed_buttons_and_escape_releases(capture_input):
    f = capture_input
    start(f, binding("win", button="right"))
    assert press(f, "middle")
    assert key_down(f, VK_ESCAPE)
    f.source.configure([WIN], False)
    assert f.native.hooks_needed
    assert release(f, "right")
    assert f.native.hooks_needed
    assert release(f, "middle")
    assert f.native.hooks_needed
    assert key_up(f, VK_ESCAPE)
    assert f.native.hooks_needed
    assert not key_up(f, 0x5B)
    assert not f.native.hooks_needed


def test_reconfigure_invalidates_queued_finish(capture_input):
    f = capture_input
    start(f)
    assert release(f)
    f.source.configure([CTRL], True)
    assert not f.source.accepts(1)
    assert not key_up(f, 0x5B)
    f.source.configure([CTRL], False)
    assert not f.native.hooks_needed
    f.source.configure([CTRL], True)
    assert f.native.hooks_needed


def test_cancel_invalidates_finished_signal_and_next_gesture_is_distinct(capture_input):
    f = capture_input
    start(f)
    assert release(f)
    f.source.cancel()
    assert not f.source.accepts(1)
    assert press(f)
    assert release(f)
    assert f.source.accepts(2)
    assert not f.source.accepts(3)


def test_reconfigure_during_drag_keeps_mouse_pair_claimed(capture_input):
    f = capture_input
    start(f)
    f.source.configure([CTRL], True)
    assert not f.source.dragging
    assert press(f)  # no second start until the real UP
    assert release(f)
    f.flush()
    assert kinds(f) == ["start", "cancel"]


def test_close_is_final_but_drains_the_swallowed_pair(capture_input):
    f = capture_input
    start(f)
    f.source.close()
    f.source.close()
    assert not f.source.accepts(1)
    assert not f.source.dragging
    # The press was swallowed; its release must be too before the hooks go.
    assert f.native.hooks_needed
    assert release(f)
    assert not key_up(f, 0x5B)
    assert f.native.mask_calls == 1
    assert not f.native.hooks_needed
    f.source.configure([WIN], True)
    assert not f.native.hooks_needed
    f.native.hold(0x5B)
    assert not press(f)
    f.native.fail("after close")
    f.flush()
    assert kinds(f) == ["start", "cancel"]
    assert not f.errors


def test_hook_failure_cancels_invalidates_and_releases_hooks(capture_input):
    f = capture_input
    start(f)
    assert press(f, "right")
    f.native.fail("hook failed")
    f.flush()
    assert f.errors == ["hook failed"]
    assert kinds(f) == ["start", "cancel"]
    assert not f.source.dragging
    assert not f.source.accepts(1)
    assert not f.native.hooks_needed
    # Claims are dropped with the hooks: the releases now belong to the desktop.
    assert not release(f)
    assert not release(f, "right")


def test_mask_failure_does_not_swallow_real_modifier_release(capture_input):
    f = capture_input
    f.native.set_mask_error("mask failed")
    start(f)
    assert f.source.dragging
    assert release(f)
    assert not key_up(f, 0x5B)
    f.flush()
    assert f.errors == ["mask failed"]
    assert kinds(f) == ["start", "finish"]
    assert not key_up(f, 0x5B)
    f.flush()
    assert f.errors == ["mask failed"]
    assert f.native.mask_calls == 1


@pytest.mark.parametrize("key", [0x5B, 0x5C])
def test_long_win_drag_masks_only_final_release_even_after_mouse_up(capture_input, key):
    f = capture_input
    start(f, keys=(key,))
    for _ in range(20):
        assert not key_down(f, key)
    assert release(f)
    f.flush()
    # Capture may already have created a pin and changed foreground focus;
    # Win autorepeat must not exhaust the pending release protection.
    for _ in range(20):
        assert not key_down(f, key)
    assert f.native.mask_calls == 0
    assert not key_up(f, key)
    assert f.native.mask_calls == 1
    f.flush()
    assert kinds(f) == ["start", "finish"]
    assert not key_up(f, key)
    assert f.native.mask_calls == 1


def test_both_win_keys_keep_independent_release_protection(capture_input):
    f = capture_input
    start(f, keys=(0x5B, 0x5C))
    assert release(f)
    assert not key_up(f, 0x5B)
    assert f.native.mask_calls == 1
    assert not key_down(f, 0x5C)
    assert not key_up(f, 0x5C)
    assert f.native.mask_calls == 2
    f.source.configure([WIN], False)
    assert not f.native.hooks_needed


def test_win_side_pressed_during_drag_keeps_protection_until_its_release(capture_input):
    f = capture_input
    start(f)
    assert not key_down(f, 0x5C)
    assert not key_up(f, 0x5B)
    assert f.source.dragging
    assert f.native.mask_calls == 1
    assert release(f)
    assert not key_down(f, 0x5C)
    assert not key_up(f, 0x5C)
    assert f.native.mask_calls == 2
    f.source.configure([WIN], False)
    assert not f.native.hooks_needed
    f.flush()
    assert kinds(f) == ["start", "finish"]


@pytest.mark.parametrize("key", [0xA4, 0xA5])
def test_completed_alt_drag_masks_release(capture_input, key):
    f = capture_input
    start(f, binding("alt"), (key,))
    assert release(f)
    assert f.native.mask_calls == 0
    assert not key_up(f, key)
    assert f.native.mask_calls == 1


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
    assert release(f)
    f.flush()
    assert f.native.hooks_needed
    assert not key_down(f, 0x5B)  # repeat after cancellation
    assert f.native.mask_calls == 0
    assert not key_up(f, 0x5B)
    f.flush()
    assert f.native.mask_calls == 1
    assert f.native.hooks_needed == (transition != "disable")


def test_disabling_after_completed_capture_waits_for_win_release(capture_input):
    f = capture_input
    start(f)
    assert release(f)
    f.flush()
    f.source.configure([WIN], False)
    assert f.native.hooks_needed
    assert not key_up(f, 0x5B)
    f.flush()
    assert f.native.mask_calls == 1
    assert not f.native.hooks_needed


@pytest.mark.parametrize("key", [0x5B, 0x5C])
def test_plain_win_key_before_and_after_capture_keeps_native_behavior(capture_input, key):
    f = capture_input
    f.source.configure([WIN], True)
    assert not key_down(f, key)
    assert not key_up(f, key)
    assert f.native.mask_calls == 0
    start(f, keys=(key,))
    assert release(f)
    assert not key_up(f, key)
    assert f.native.mask_calls == 1
    assert not key_down(f, key)
    assert not key_up(f, key)
    assert f.native.mask_calls == 1


def test_injected_win_release_does_not_consume_physical_release_protection(capture_input):
    f = capture_input
    start(f)
    assert release(f)
    assert not key_up(f, 0x5B, injected=True)
    assert f.native.mask_calls == 0
    assert not key_down(f, 0x5B)
    assert not key_up(f, 0x5B)
    assert f.native.mask_calls == 1


def test_unrelated_menu_key_release_is_not_masked(capture_input):
    f = capture_input
    start(f)
    assert release(f)
    assert not key_up(f, 0x5C)
    assert not key_up(f, 0xA4)
    assert f.native.mask_calls == 0
    assert not key_up(f, 0x5B)
    assert f.native.mask_calls == 1


def test_other_keys_during_drag_reach_the_desktop(capture_input):
    f = capture_input
    start(f)
    for key in (0x43, 0xBB, 0xA1):
        assert not key_down(f, key)
        assert not key_up(f, key)
    assert f.source.dragging


def test_gesture_binding_is_reported_as_a_modifier_set(capture_input):
    f = capture_input
    start(f, binding("shift", "ctrl", button="right"), (0xA0, 0xA2))
    assert f.source.gesture_binding(1) == (frozenset({"ctrl", "shift"}), "right")
    assert f.source.gesture_binding(2) is None
