"""真实输入测试的公共部分：带测试标记的 SendInput、记录自己收到了什么的 Tk 靶窗口、看门狗。

靶窗口在独立进程里，置顶显示在屏幕右侧，按行记录收到的输入：press / release / wheel、key <keysym>。
"""

import contextlib
import ctypes
import os
import subprocess
import sys
import time
from ctypes import wintypes
from types import SimpleNamespace

import pytest

requires_real_input = [
    pytest.mark.skipif(
        sys.platform != "win32" or os.environ.get("RUN_REAL_INPUT_TESTS") != "1",
        reason="drives the real mouse and keyboard; set RUN_REAL_INPUT_TESTS=1",
    ),
    pytest.mark.real_input_hub,
]

MARKER = 0x4A544241
VK_LCONTROL, VK_LWIN, VK_ESCAPE = 0xA2, 0x5B, 0x1B
LEFT_DOWN, LEFT_UP, MIDDLE_DOWN, MIDDLE_UP = 0x0002, 0x0004, 0x0020, 0x0040
X_DOWN, X_UP, WHEEL = 0x0080, 0x0100, 0x0800

TARGET = r"""
import ctypes, sys, tkinter as tk
ctypes.windll.user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
log = open(sys.argv[1], "a", encoding="utf-8", buffering=1)
root = tk.Tk()
root.title("JIETUBA-REAL-INPUT")
root.attributes("-topmost", True)
root.geometry(f"520x360+{root.winfo_screenwidth() - 640 - 560 * int(sys.argv[2])}+200")
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


def mouse(x, y, flags=0, data=0, marker=MARKER):
    left, top, width, height = (user32.GetSystemMetrics(index) for index in (76, 77, 78, 79))
    # MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE
    return INPUT(0, _InputUnion(mi=MOUSEINPUT(
        (x - left) * 65535 // (width - 1), (y - top) * 65535 // (height - 1), data & 0xFFFFFFFF,
        flags | 0x4000 | 0x8000 | 0x0001, 0, marker)))


def click(x, y):
    send(mouse(x, y), mouse(x, y, LEFT_DOWN), mouse(x, y, LEFT_UP))


def key(vk, up=False):
    return INPUT(1, _InputUnion(ki=KEYBDINPUT(vk, 0, 2 if up else 0, 0, MARKER)))


def foreground_class():
    buffer = ctypes.create_unicode_buffer(128)
    user32.GetClassNameW(user32.GetForegroundWindow(), buffer, 128)
    return buffer.value


@contextlib.contextmanager
def real_desktop(qtbot, tmp_path, windows=1):
    """打开靶窗口、切到每显示器 DPI 感知，并在测试超时时结束整个进程。"""
    # 钩子线程若锁死，系统的每个输入都会变慢，收尾也会卡住；直接结束这次运行
    watchdog = subprocess.Popen([sys.executable, "-c", "import os, sys, time; time.sleep(30); os.kill(int(sys.argv[1]), 9)",
                                 str(os.getpid())])
    previous = user32.SetThreadDpiAwarenessContext(ctypes.c_void_p(-4))
    processes, targets = [], []
    try:
        for index in range(windows):
            log = tmp_path / f"target{index}.log"
            processes.append(subprocess.Popen([sys.executable, "-c", TARGET, str(log), str(index)]))
            qtbot.waitUntil(lambda log=log: log.exists() and "READY" in log.read_text(encoding="utf-8"), timeout=10000)
            ready = next(line for line in log.read_text(encoding="utf-8").splitlines() if line.startswith("READY"))
            frame, left, top, width, height = map(int, ready.split()[1:])

            def seen(log=log):
                return [line for line in log.read_text(encoding="utf-8").splitlines() if not line.startswith("READY")]

            targets.append(SimpleNamespace(frame=frame, center=(left + width // 2, top + height // 2), seen=seen))
        yield targets
    finally:
        from core.input_hub import close_input_hub
        close_input_hub()
        for process in processes:
            process.terminate()
            process.wait(5)
        user32.SetThreadDpiAwarenessContext(ctypes.c_void_p(previous))
        watchdog.kill()
