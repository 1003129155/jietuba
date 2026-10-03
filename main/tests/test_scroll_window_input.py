"""长截图的全局输入：截图区域内的滚轮、累计的滚动方向、横向模式下的 Shift。

不实例化 ScrollCaptureWindow（__init__ 会建窗口和定时器），用只带所需属性的替身调用这几个方法；
输入中心是不装钩子的原生状态机（见 conftest），滚轮和按键由测试逐条给出。
"""

from types import SimpleNamespace
from unittest.mock import Mock

import pytest
from PySide6.QtCore import QRect, QTimer

from core.input_hub import existing_input_hub, input_hub
from stitch.scroll_window import ScrollCaptureWindow

VK_LSHIFT, VK_RSHIFT = 0xA0, 0xA1
INSIDE, OUTSIDE = (150, 250), (50, 250)


class FakeWindow:
    _setup_mouse_hook = ScrollCaptureWindow._setup_mouse_hook
    _on_wheel = ScrollCaptureWindow._on_wheel
    _on_key = ScrollCaptureWindow._on_key
    _start_keyboard_listener = ScrollCaptureWindow._start_keyboard_listener
    _stop_keyboard_listener = ScrollCaptureWindow._stop_keyboard_listener
    _cleanup = ScrollCaptureWindow._cleanup

    def __init__(self, direction="vertical"):
        self.capture_rect = QRect(100, 200, 300, 400)
        self.scroll_direction = direction
        self._pending_steps = 0.0
        self.horizontal_scroll_key_pressed = False
        self.scroll_cooldown = 0
        self._input_hub = None
        self.scrolls = []
        self.scroll_detected = SimpleNamespace(emit=self.scrolls.append)
        self.transparent_area = SimpleNamespace(winId=Mock(side_effect=RuntimeError("offscreen")))
        self._send_horizontal_scroll = Mock()
        self._capture_when_still = Mock()
        self.capture_timer = QTimer()
        self.capture_timer.setSingleShot(True)
        self.capture_timer.timeout.connect(lambda: self._capture_when_still())


@pytest.fixture
def window(qapp):
    window = FakeWindow()
    window._setup_mouse_hook()
    yield window
    window._cleanup()


def wheel(qapp, delta, at=INSIDE, horizontal=False):
    input_hub().native.mouse("hwheel" if horizontal else "wheel", *at, delta=delta)
    qapp.processEvents()


def key(qapp, vk, pressed):
    input_hub().native.key(vk, pressed)
    qapp.processEvents()


def test_only_wheels_inside_the_capture_area_count(qapp, window):
    wheel(qapp, -120, at=OUTSIDE)
    assert window.scrolls == []
    wheel(qapp, -120)
    wheel(qapp, -240)
    assert window.scrolls == [25, 50]


def test_vertical_mode_captures_both_directions_and_tracks_the_net_scroll(qapp, window):
    wheel(qapp, 120)
    wheel(qapp, -120)  # 往回滚同样截图
    wheel(qapp, 120, horizontal=True)  # 竖向模式不管横向滚轮
    wheel(qapp, 120)
    assert window.scrolls == [25, 25, 25]
    assert window._pending_steps == -1  # 净往上一格


def test_small_touchpad_steps_count_in_both_directions(qapp, window):
    wheel(qapp, 8)
    wheel(qapp, 30)
    assert window.scrolls == [1, 6]
    assert window._pending_steps < 0


def test_horizontal_mode_takes_the_horizontal_wheel_and_shift_wheel(qapp, window):
    window.scroll_direction = "horizontal"
    wheel(qapp, 120, horizontal=True)  # 向右
    wheel(qapp, -120)  # 竖向滚轮向下（含 Shift+滚轮）算向右
    wheel(qapp, -120, horizontal=True)  # 向左同样截图
    assert window.scrolls == [25, 25, 25]
    assert window._pending_steps == 1  # 净往右一格


def test_shift_in_horizontal_mode_scrolls_and_captures_once_per_press(qapp, qtbot, window):
    window.scroll_direction = "horizontal"
    window._start_keyboard_listener()
    key(qapp, VK_LSHIFT, True)
    key(qapp, VK_LSHIFT, True)  # 自动重复
    qtbot.waitUntil(lambda: window._capture_when_still.call_count == 1)
    assert window._send_horizontal_scroll.call_count == 1
    assert window._pending_steps == 1
    key(qapp, VK_LSHIFT, False)
    key(qapp, VK_RSHIFT, True)
    qtbot.waitUntil(lambda: window._capture_when_still.call_count == 2)
    assert window._send_horizontal_scroll.call_count == 2


def test_shift_is_not_watched_outside_horizontal_mode(qapp, window):
    window.scroll_direction = "horizontal"
    window._start_keyboard_listener()
    window.scroll_direction = "vertical"
    window._stop_keyboard_listener()
    key(qapp, VK_LSHIFT, True)
    assert not window._send_horizontal_scroll.called


def test_cleanup_releases_the_hooks(qapp, window):
    window.scroll_direction = "horizontal"
    window._start_keyboard_listener()
    assert input_hub().native.hooks_needed
    window._cleanup()
    assert not existing_input_hub().native.hooks_needed
    wheel(qapp, -120)
    assert window.scrolls == []
