"""Quick capture through the real system hooks: injected input carrying the hub's test marker.

These tests move the cursor, click and press keys on the real desktop, so they only run with
RUN_REAL_INPUT_TESTS=1 (see tests/real_input.py). Another program whose own hook grabs the same
gesture first can make them fail.
"""

import json
import subprocess
import sys
import time
from types import SimpleNamespace

import pytest
from PySide6.QtCore import Qt

from tests.real_input import (
    LEFT_DOWN, LEFT_UP, MIDDLE_DOWN, MIDDLE_UP, VK_ESCAPE, VK_LCONTROL, VK_LWIN, WHEEL,
    MARKER, click, foreground_class, key, mouse, real_desktop, requires_real_input, send, user32,
)

pytestmark = requires_real_input

MOVER = r"""
import ctypes, json, sys, time
from ctypes import wintypes
user32 = ctypes.windll.user32
user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", wintypes.LONG), ("dy", wintypes.LONG), ("mouseData", wintypes.DWORD),
                ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD), ("dwExtraInfo", ctypes.c_size_t)]
class INPUT(ctypes.Structure):
    _fields_ = [("type", wintypes.DWORD), ("mi", MOUSEINPUT)]
left, top, width, height = (user32.GetSystemMetrics(i) for i in (76, 77, 78, 79))
points, seconds = json.loads(sys.argv[1]), float(sys.argv[2])
samples, end, i = [], time.perf_counter() + seconds, 0
while time.perf_counter() < end:
    x, y = points[i % 2]
    i += 1
    move = INPUT(0, MOUSEINPUT((x - left) * 65535 // (width - 1), (y - top) * 65535 // (height - 1),
                               0, 0xC001, 0, 0))
    started = time.perf_counter()
    assert user32.SendInput(1, ctypes.byref(move), ctypes.sizeof(INPUT)) == 1
    pt = wintypes.POINT()
    while time.perf_counter() - started < 3:
        user32.GetCursorPos(ctypes.byref(pt))
        if (pt.x, pt.y) == (x, y):
            break
    samples.append((started, (time.perf_counter() - started) * 1000))
    time.sleep(0.003)
print(json.dumps(samples))
"""


def timed_sum(items):
    started = time.perf_counter()
    sum(range(items))
    return time.perf_counter() - started


@pytest.fixture
def desktop(qapp, qtbot, tmp_path):
    from core.input_hub import input_hub
    from core.quick_capture_input import QuickCaptureInput

    with real_desktop(qtbot, tmp_path) as (target,):
        hub = input_hub()
        hub.native.set_test_marker(MARKER)
        source = QuickCaptureInput(hub=hub)
        events, moves = [], []
        source.event.connect(lambda *args: events.append(args), Qt.ConnectionType.QueuedConnection)
        source.moved.connect(moves.append, Qt.ConnectionType.QueuedConnection)
        click(*target.center)
        qtbot.waitUntil(lambda: user32.GetForegroundWindow() == target.frame and target.seen()[-1:] == ["release"],
                        timeout=3000)
        yield SimpleNamespace(source=source, hub=hub, events=events, moves=moves, frame=target.frame,
                              center=target.center, seen=target.seen, qtbot=qtbot, qapp=qapp)
        source.close()


def test_ctrl_drag_is_reported_and_never_reaches_the_window(desktop):
    d = desktop
    d.source.configure([(frozenset({"ctrl"}), "left")], True)
    d.qtbot.waitUntil(lambda: d.hub.native.stats()["hooks_installed"], timeout=2000)
    x, y = d.center
    send(mouse(x, y, LEFT_DOWN), mouse(x, y, LEFT_UP))
    d.qtbot.waitUntil(lambda: d.seen()[-2:] == ["press", "release"], timeout=2000)
    before = len(d.seen())
    send(key(VK_LCONTROL))
    send(mouse(x, y, LEFT_DOWN))
    send(mouse(x + 30, y + 20), mouse(x + 60, y + 40))
    send(mouse(x + 60, y + 40, WHEEL, 120))
    d.qtbot.waitUntil(lambda: d.moves == [1], timeout=2000)
    assert d.source.take_position(1) == (x + 60, y + 40)
    send(mouse(x + 60, y + 40, LEFT_UP))
    send(key(VK_LCONTROL, up=True))
    d.qtbot.waitUntil(lambda: len(d.events) == 2, timeout=2000)
    assert d.events == [("start", 1, x, y), ("finish", 1, x + 60, y + 40)]
    assert d.source.accepts(1)
    assert d.source.gesture_binding(1) == (frozenset({"ctrl"}), "left")
    # Ctrl itself reaches the window; the drag and the wheel do not.
    assert d.seen()[before:] == ["key Control_L", "keyup Control_L"]


def test_remote_drag_and_escape_work_without_test_marker(desktop):
    """Ordinary injected events, as sent by remote-control tools, need no bypass."""
    d = desktop
    d.hub.native.set_test_marker(0)
    d.source.configure([(frozenset({"ctrl"}), "left")], True)
    d.qtbot.waitUntil(lambda: d.hub.native.stats()["hooks_installed"], timeout=2000)
    x, y = d.center
    send(key(VK_LCONTROL))
    send(mouse(x, y, LEFT_DOWN))
    d.qtbot.waitUntil(lambda: len(d.events) == 1, timeout=2000)
    send(mouse(x + 40, y + 30))
    d.qtbot.waitUntil(lambda: d.moves == [1], timeout=2000)
    assert d.source.take_position(1) == (x + 40, y + 30)
    send(mouse(x + 40, y + 30, LEFT_UP), key(VK_LCONTROL, up=True))
    d.qtbot.waitUntil(lambda: len(d.events) == 2, timeout=2000)
    assert d.events == [("start", 1, x, y), ("finish", 1, x + 40, y + 30)]
    send(key(VK_LCONTROL), mouse(x, y, LEFT_DOWN))
    send(key(VK_ESCAPE), key(VK_ESCAPE, up=True))
    send(mouse(x, y, LEFT_UP), key(VK_LCONTROL, up=True))
    d.qtbot.waitUntil(lambda: len(d.events) == 4, timeout=2000)
    assert [event[0] for event in d.events] == ["start", "finish", "start", "cancel"]
    assert not d.source.dragging
    assert not d.source.accepts(2)


def test_escape_cancels_and_both_pairs_are_swallowed(desktop):
    d = desktop
    d.source.configure([(frozenset({"ctrl"}), "left")], True)
    before = len(d.seen())
    x, y = d.center
    send(key(VK_LCONTROL))
    send(mouse(x, y, LEFT_DOWN))
    send(key(VK_ESCAPE), key(VK_ESCAPE, up=True))
    send(mouse(x, y, LEFT_UP))
    send(key(VK_LCONTROL, up=True))
    d.qtbot.waitUntil(lambda: len(d.events) == 2, timeout=2000)
    assert [event[0] for event in d.events] == ["start", "cancel"]
    assert not d.source.accepts(1)
    assert d.seen()[before:] == ["key Control_L", "keyup Control_L"]


@pytest.mark.parametrize("marker", [MARKER, 0])
def test_win_drag_release_does_not_open_the_start_menu(desktop, marker):
    d = desktop
    d.hub.native.set_test_marker(marker)
    # The middle button: Win + left drag is a common binding in other capture tools.
    d.source.configure([(frozenset({"win"}), "middle")], True)
    x, y = d.center
    send(key(VK_LWIN))
    send(mouse(x, y, MIDDLE_DOWN), mouse(x + 50, y + 30), mouse(x + 50, y + 30, MIDDLE_UP))
    send(key(VK_LWIN, up=True))
    time.sleep(0.5)
    foreground, name = user32.GetForegroundWindow(), foreground_class()
    if name == "Windows.UI.Core.CoreWindow":
        send(key(VK_ESCAPE), key(VK_ESCAPE, up=True))  # the Start menu opened; close only that
    d.qtbot.waitUntil(lambda: len(d.events) == 2, timeout=2000)
    assert [event[0] for event in d.events] == ["start", "finish"], "another program's hook took the gesture"
    assert (foreground, name) == (d.frame, "TkTopLevel")


def test_busy_gui_thread_does_not_stall_the_system_cursor(desktop):
    d = desktop
    d.source.configure([(frozenset({"ctrl"}), "left")], True)
    d.qtbot.waitUntil(lambda: d.hub.native.stats()["hooks_installed"], timeout=2000)
    x, y = d.center
    mover = subprocess.Popen([sys.executable, "-c", MOVER, json.dumps([[x - 40, y], [x + 40, y]]), "1.2"],
                             stdout=subprocess.PIPE, text=True)
    # sum() over a range runs in C without releasing the GIL.
    fastest = min(timed_sum(1_000_000) for _ in range(3))
    items = int(0.3 / fastest * 1_000_000)
    time.sleep(0.4)
    started = time.perf_counter()
    sum(range(items))
    ended = time.perf_counter()
    output, _ = mover.communicate(timeout=10)
    during = [latency for at, latency in json.loads(output) if started <= at <= ended]
    assert ended - started > 0.2
    assert during
    assert max(during) < 50, f"cursor stalled {max(during):.0f} ms while the GUI thread held the GIL"
    assert len(during) > 20
