"""侧键热键与剪贴板粘贴目标跟踪，走真实的系统钩子（RUN_REAL_INPUT_TESTS=1，见 tests/real_input.py）。"""

import time

from clipboard.controllers.foreground_tracker import ForegroundWindowTracker
from core.input_hub import input_hub
from core.shortcut_manager import MOUSE_BUTTON_BACK, ShortcutManager
from tests.real_input import (
    MARKER, VK_ESCAPE, VK_LWIN, X_DOWN, X_UP, click, foreground_class, key, mouse, real_desktop,
    requires_real_input, send, user32,
)

pytestmark = requires_real_input


def test_bound_side_button_is_swallowed_and_dispatched_while_the_other_passes(qapp, qtbot, tmp_path):
    with real_desktop(qtbot, tmp_path) as (target,):
        manager = ShortcutManager()
        calls = []
        manager.register_hotkey(MOUSE_BUTTON_BACK, lambda: calls.append("back"))
        try:
            qtbot.waitUntil(lambda: input_hub().native.stats()["hooks_installed"], timeout=2000)
            click(*target.center)
            qtbot.waitUntil(lambda: target.seen()[-1:] == ["release"], timeout=2000)
            before = len(target.seen())
            x, y = target.center
            send(mouse(x, y, X_DOWN, 1), mouse(x, y, X_UP, 1))
            qtbot.waitUntil(lambda: calls == ["back"], timeout=2000)
            send(mouse(x, y, X_DOWN, 2), mouse(x, y, X_UP, 2))
            time.sleep(0.2)
            # 只有没绑定的前进键到了窗口
            assert target.seen()[before:] == ["press", "release"]
            assert calls == ["back"]
        finally:
            manager.unregister_all_hotkeys()
            ShortcutManager._registered_mouse_buttons_global.clear()
        # 钩子由原生线程异步装卸
        qtbot.waitUntil(lambda: not input_hub().native.stats()["hooks_installed"], timeout=2000)


def test_taken_over_win_v_reaches_neither_the_window_nor_the_system(qapp, qtbot, tmp_path):
    with real_desktop(qtbot, tmp_path) as (target,):
        hub = input_hub()
        hub.native.set_test_marker(MARKER)
        manager = ShortcutManager()
        calls = []
        manager.register_hook_hotkey("clipboard", ["win"], ord("V"), lambda: calls.append("clipboard"))
        try:
            qtbot.waitUntil(lambda: hub.native.stats()["hooks_installed"], timeout=2000)
            click(*target.center)
            qtbot.waitUntil(lambda: user32.GetForegroundWindow() == target.frame
                            and target.seen()[-1:] == ["release"], timeout=2000)
            before = len(target.seen())
            # 第二个按下是长按的自动重复
            send(key(VK_LWIN), key(ord("V")), key(ord("V")), key(ord("V"), up=True), key(VK_LWIN, up=True))
            time.sleep(0.5)
            foreground, name = user32.GetForegroundWindow(), foreground_class()
            if name == "Windows.UI.Core.CoreWindow":
                send(key(VK_ESCAPE), key(VK_ESCAPE, up=True))  # 开始菜单或系统剪贴板弹出了，只关掉它
            qtbot.waitUntil(lambda: calls == ["clipboard"], timeout=2000)
            assert (foreground, name) == (target.frame, "TkTopLevel")
            # 窗口只收到 Win 和松开前补发的一对未分配键（Tk 记作 ??）
            assert target.seen()[before:] == ["key Win_L", "key ??", "keyup ??", "keyup Win_L"]
            # 松开 Win 之后的 V 照常送达；输入法开着时按下会记成 ??，只看抬起
            send(key(ord("V")), key(ord("V"), up=True))
            qtbot.waitUntil(lambda: "keyup v" in target.seen()[before:], timeout=2000)
            assert calls == ["clipboard"]
        finally:
            manager.unregister_all_hotkeys()
        qtbot.waitUntil(lambda: not hub.native.stats()["hooks_installed"], timeout=2000)


def test_paste_target_catches_a_quick_switch_and_skips_the_picker(qapp, qtbot, tmp_path):
    with real_desktop(qtbot, tmp_path, windows=2) as (picker, other):
        tracker = ForegroundWindowTracker()
        tracker.set_excluded(lambda hwnd: hwnd == picker.frame)
        click(*picker.center)
        qtbot.waitUntil(lambda: user32.GetForegroundWindow() == picker.frame, timeout=2000)
        tracker.start()
        try:
            qtbot.waitUntil(lambda: input_hub().native.stats()["foreground_hook_installed"], timeout=2000)
            # 切过去点一下立刻切回拾取窗口，只停留几十毫秒
            click(*other.center)
            click(*picker.center)
            qtbot.waitUntil(lambda: tracker.target_hwnd == other.frame, timeout=2000)
            assert user32.GetForegroundWindow() == picker.frame
        finally:
            tracker.stop()
        qtbot.waitUntil(lambda: not input_hub().native.stats()["foreground_hook_installed"], timeout=2000)
