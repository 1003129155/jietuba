"""长截图的全局输入：横向模式下的 Shift。截图不看滚轮，交给画面监视。

不实例化 ScrollCaptureWindow（__init__ 会建窗口和定时器），用只带所需属性的替身调用这几个方法；
输入中心是不装钩子的原生状态机（见 conftest），按键由测试逐条给出。
"""

from types import SimpleNamespace
from unittest.mock import Mock

import pytest
from PySide6.QtCore import QRect

from core.input_hub import existing_input_hub, input_hub
from stitch.scroll_window import ScrollCaptureWindow

VK_LSHIFT, VK_RSHIFT = 0xA0, 0xA1


class FakeWindow:
    _setup_input = ScrollCaptureWindow._setup_input
    _on_key = ScrollCaptureWindow._on_key
    _start_keyboard_listener = ScrollCaptureWindow._start_keyboard_listener
    _stop_keyboard_listener = ScrollCaptureWindow._stop_keyboard_listener
    _cleanup = ScrollCaptureWindow._cleanup

    def __init__(self, direction="vertical"):
        self.capture_rect = QRect(100, 200, 300, 400)
        self.scroll_direction = direction
        self.horizontal_scroll_key_pressed = False
        self._input_hub = None
        self.transparent_area = SimpleNamespace(winId=Mock(side_effect=RuntimeError("offscreen")))
        self._send_horizontal_scroll = Mock()


@pytest.fixture
def window(qapp):
    window = FakeWindow()
    window._setup_input()
    yield window
    window._cleanup()


def key(qapp, vk, pressed):
    input_hub().native.key(vk, pressed)
    qapp.processEvents()


def test_shift_in_horizontal_mode_scrolls_once_per_press(qapp, qtbot, window):
    window.scroll_direction = "horizontal"
    window._start_keyboard_listener()
    key(qapp, VK_LSHIFT, True)
    key(qapp, VK_LSHIFT, True)  # 自动重复
    qtbot.waitUntil(lambda: window._send_horizontal_scroll.call_count == 1)
    key(qapp, VK_LSHIFT, False)
    key(qapp, VK_RSHIFT, True)
    qtbot.waitUntil(lambda: window._send_horizontal_scroll.call_count == 2)


def test_shift_is_not_watched_outside_horizontal_mode(qapp, window):
    window.scroll_direction = "horizontal"
    window._start_keyboard_listener()
    window.scroll_direction = "vertical"
    window._stop_keyboard_listener()
    key(qapp, VK_LSHIFT, True)
    assert not window._send_horizontal_scroll.called


def test_the_wheel_is_not_watched(qapp, window):
    assert not input_hub().native.hooks_needed


def test_cleanup_releases_the_hooks(qapp, window):
    window.scroll_direction = "horizontal"
    window._start_keyboard_listener()
    assert input_hub().native.hooks_needed
    window._cleanup()
    assert not existing_input_hub().native.hooks_needed
    key(qapp, VK_LSHIFT, True)
    assert not window._send_horizontal_scroll.called
