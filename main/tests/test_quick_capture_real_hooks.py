"""Quick capture through the real system hooks: injected input carrying the hub's test marker.

These tests move the cursor, click and press keys on the real desktop, so they only run with
RUN_REAL_INPUT_TESTS=1. A separate topmost Tk window records what actually reaches the desktop.
Another program whose own hook grabs the same gesture first can make them fail.
"""

import ctypes
import json
import os
import subprocess
import sys
import time
from ctypes import wintypes
from types import SimpleNamespace

import pytest
from PySide6.QtCore import Qt

pytestmark = pytest.mark.skipif(
    sys.platform != "win32" or os.environ.get("RUN_REAL_INPUT_TESTS") != "1",
    reason="drives the real mouse and keyboard; set RUN_REAL_INPUT_TESTS=1",
)

MARKER = 0x4A544241
VK_LCONTROL, VK_LWIN, VK_ESCAPE = 0xA2, 0x5B, 0x1B
LEFT_DOWN, LEFT_UP, MIDDLE_DOWN, MIDDLE_UP, WHEEL = 0x0002, 0x0004, 0x0020, 0x0040, 0x0800
PER_MONITOR_AWARE_V2 = ctypes.c_void_p(-4)

TARGET = r"""
import ctypes, sys, tkinter as tk
ctypes.windll.user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
log = open(sys.argv[1], "a", encoding="utf-8", buffering=1)
root = tk.Tk()
root.title("JIETUBA-REAL-INPUT")
root.attributes("-topmost", True)
root.geometry(f"520x360+{root.winfo_screenwidth() - 640}+200")
for sequence, kind in (("<ButtonPress>", "press"), ("<ButtonRelease>", "release"), ("<MouseWheel>", "wheel"),
                       ("<KeyPress>", "key"), ("<KeyRelease>", "keyup")):
    root.bind(sequence, lambda event, kind=kind: log.write(
        kind + (" " + event.keysym if kind.startswith("key") else "") + "\n"))
def ready():
    frame = ctypes.windll.user32.GetAncestor(root.winfo_id(), 2)
    log.write(f"READY {frame} {root.winfo_rootx()} {root.winfo_rooty()} {root.winfo_width()} {root.winfo_height()}\n")
root.after(400, ready)
root.mainloop()
"""

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


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", wintypes.LONG), ("dy", wintypes.LONG), ("mouseData", wintypes.DWORD),
                ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD), ("dwExtraInfo", ctypes.c_size_t)]


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD), ("dwFlags", wintypes.DWORD),
                ("time", wintypes.DWORD), ("dwExtraInfo", ctypes.c_size_t)]


class _InputUnion(ctypes.Union):
    _fields_ = [("mi", MOUSEINPUT), ("ki", KEYBDINPUT)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", wintypes.DWORD), ("value", _InputUnion)]


user32 = ctypes.WinDLL("user32", use_last_error=True) if sys.platform == "win32" else None


def send(*inputs):
    array = (INPUT * len(inputs))(*inputs)
    assert user32.SendInput(len(inputs), array, ctypes.sizeof(INPUT)) == len(inputs)
    time.sleep(0.03)


def mouse(x, y, flags=0, data=0):
    left, top, width, height = (user32.GetSystemMetrics(index) for index in (76, 77, 78, 79))
    # MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE
    return INPUT(0, _InputUnion(mi=MOUSEINPUT(
        (x - left) * 65535 // (width - 1), (y - top) * 65535 // (height - 1), data & 0xFFFFFFFF,
        flags | 0x4000 | 0x8000 | 0x0001, 0, MARKER)))


def key(vk, up=False):
    return INPUT(1, _InputUnion(ki=KEYBDINPUT(vk, 0, 2 if up else 0, 0, MARKER)))


def timed_sum(items):
    started = time.perf_counter()
    sum(range(items))
    return time.perf_counter() - started


def foreground_class():
    buffer = ctypes.create_unicode_buffer(128)
    user32.GetClassNameW(user32.GetForegroundWindow(), buffer, 128)
    return buffer.value


@pytest.fixture
def desktop(qapp, qtbot, tmp_path):
    from core.input_hub import close_input_hub, input_hub
    from core.quick_capture_input import QuickCaptureInput

    # A deadlocked hook thread slows every input on the system and hangs teardown; end the run instead.
    watchdog = subprocess.Popen([sys.executable, "-c", "import os, sys, time; time.sleep(30); os.kill(int(sys.argv[1]), 9)",
                                 str(os.getpid())])
    previous = user32.SetThreadDpiAwarenessContext(PER_MONITOR_AWARE_V2)
    log = tmp_path / "target.log"
    target = subprocess.Popen([sys.executable, "-c", TARGET, str(log)])
    try:
        qtbot.waitUntil(lambda: log.exists() and "READY" in log.read_text(encoding="utf-8"), timeout=10000)
        ready = next(line for line in log.read_text(encoding="utf-8").splitlines() if line.startswith("READY"))
        frame, left, top, width, height = map(int, ready.split()[1:])
        hub = input_hub()
        hub.native.set_test_marker(MARKER)
        source = QuickCaptureInput(hub=hub)
        events, moves = [], []
        source.event.connect(lambda *args: events.append(args), Qt.ConnectionType.QueuedConnection)
        source.moved.connect(moves.append, Qt.ConnectionType.QueuedConnection)
        center = (left + width // 2, top + height // 2)

        def seen():
            return [line for line in log.read_text(encoding="utf-8").splitlines() if not line.startswith("READY")]

        send(mouse(*center), mouse(*center, LEFT_DOWN), mouse(*center, LEFT_UP))
        qtbot.waitUntil(lambda: user32.GetForegroundWindow() == frame and seen()[-1:] == ["release"], timeout=3000)
        yield SimpleNamespace(source=source, hub=hub, events=events, moves=moves, frame=frame, center=center,
                              seen=seen, qtbot=qtbot, qapp=qapp)
        source.close()
    finally:
        close_input_hub()
        target.terminate()
        target.wait(5)
        user32.SetThreadDpiAwarenessContext(ctypes.c_void_p(previous))
        watchdog.kill()


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


def test_win_drag_release_does_not_open_the_start_menu(desktop):
    d = desktop
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
