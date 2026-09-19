# -*- coding: utf-8 -*-
"""验证"选择后自动粘贴"开关只管选中触发的粘贴，不管右键菜单的显式粘贴。"""

import pytest

from clipboard.controllers import clipboard_controller as controller_module
from clipboard.controllers.clipboard_controller import ClipboardController


class DummyClipboardManager:
    pass


@pytest.fixture
def controller(monkeypatch):
    monkeypatch.setattr(ClipboardController, "_load_settings", lambda self: None)
    ctrl = ClipboardController(DummyClipboardManager())
    ctrl.paste_with_html = True
    return ctrl


def _capture_keystrokes(monkeypatch):
    """把 QTimer.singleShot 换成立即执行，记录 send_ctrl_v 被调用几次。"""
    calls = []
    monkeypatch.setattr(controller_module.QTimer, "singleShot", lambda _ms, fn: fn())
    monkeypatch.setattr(controller_module, "set_foreground_window", lambda _hwnd: None)
    monkeypatch.setattr(controller_module, "send_ctrl_v", lambda: calls.append(1))
    return calls


@pytest.mark.parametrize(
    "auto_paste_enabled, explicit, expected",
    [
        (True, False, 1),    # 开关开着，双击选中 → 自动粘
        (False, False, 0),   # 开关关掉，双击选中 → 只复制，不粘
        (True, True, 1),     # 开关开着，右键粘贴 → 粘
        (False, True, 1),    # 开关关掉，右键粘贴 → 仍然粘（这是手动动作，不受开关约束）
    ],
)
def test_paste_keystroke_gating(monkeypatch, controller, auto_paste_enabled, explicit, expected):
    calls = _capture_keystrokes(monkeypatch)
    controller.auto_paste_enabled = auto_paste_enabled
    controller._previous_window_hwnd = 1234

    controller._send_paste_keystroke(explicit)

    assert len(calls) == expected
def test_right_click_paste_sends_ctrl_v_with_auto_paste_off(monkeypatch, controller):
    """走完整的 paste_item，确认 explicit 真的从入参传到了按键那一步。"""
    import settings

    calls = _capture_keystrokes(monkeypatch)

    class _Config:
        def get_clipboard_move_to_top_on_paste(self):
            return False

    monkeypatch.setattr(settings, "get_tool_settings_manager", lambda: _Config())

    class _Manager:
        def paste_item(self, item_id, with_html, move_to_top):
            return True

    controller.manager = _Manager()
    controller.auto_paste_enabled = False
    controller._previous_window_hwnd = 1234

    assert controller.paste_item(1, explicit=True) is True
    assert len(calls) == 1

    assert controller.paste_item(1) is True  # 同一条路径，非显式 → 不补按键
    assert len(calls) == 1
