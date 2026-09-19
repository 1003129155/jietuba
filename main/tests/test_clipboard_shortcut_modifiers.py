# -*- coding: utf-8 -*-
"""数字/字母直选条目只认裸按键。

V 落在 A~Z 区间里，带 Ctrl 也照单全收的话，本应用自己模拟出来的 Ctrl+V 会在
窗口常驻时回到这个处理器，被当成「粘贴第 31 条」——粘贴再触发一次模拟按键，
就成了停不下来的自激循环。
"""

from types import SimpleNamespace

import pytest
from PySide6.QtCore import Qt
from PySide6.QtWidgets import QApplication

from clipboard.ui.windows.clipboard_window import ClipboardShortcutHandler


class _Event:
    def __init__(self, key, modifiers=Qt.KeyboardModifier.NoModifier):
        self._key = key
        self._modifiers = modifiers

    def key(self):
        return self._key

    def modifiers(self):
        return self._modifiers


class _Window:
    def __init__(self, item_count=40):
        self.search_input = SimpleNamespace(setFocus=lambda: setattr(self, "search_focused", True))
        self.search_focused = False
        self.selection_manager = SimpleNamespace(_selected_index=-1)
        self.controller = SimpleNamespace(
            current_items=[SimpleNamespace(id=100 + i) for i in range(item_count)]
        )
        self.pasted = []

    def _on_paste_item(self, item_id):
        self.pasted.append(item_id)


@pytest.fixture
def handler(monkeypatch):
    """焦点不在搜索框上，直选快捷键才会生效。"""
    monkeypatch.setattr(QApplication, "focusWidget", staticmethod(lambda: None))
    window = _Window()
    return ClipboardShortcutHandler(window), window


def test_bare_letter_picks_an_item(handler):
    shortcut, window = handler

    assert shortcut.handle_key(_Event(Qt.Key.Key_V)) is True
    assert window.pasted == [100 + 30]  # 9 + (V - A)


def test_ctrl_v_is_not_a_direct_pick(handler):
    """这正是自激循环的入口：模拟出来的 Ctrl+V 不能被当成直选。"""
    shortcut, window = handler

    assert shortcut.handle_key(_Event(Qt.Key.Key_V, Qt.KeyboardModifier.ControlModifier)) is False
    assert window.pasted == []


def test_bare_digit_picks_an_item(handler):
    shortcut, window = handler

    assert shortcut.handle_key(_Event(Qt.Key.Key_2)) is True
    assert window.pasted == [100 + 1]


def test_ctrl_digit_is_not_a_direct_pick(handler):
    """Ctrl+2 是唤出窗口的全局热键，不该顺手粘贴第二条。"""
    shortcut, window = handler

    assert shortcut.handle_key(_Event(Qt.Key.Key_2, Qt.KeyboardModifier.ControlModifier)) is False
    assert window.pasted == []


@pytest.mark.parametrize(
    "modifier",
    [Qt.KeyboardModifier.AltModifier, Qt.KeyboardModifier.MetaModifier],
)
def test_other_command_modifiers_are_also_blocked(handler, modifier):
    shortcut, window = handler

    assert shortcut.handle_key(_Event(Qt.Key.Key_V, modifier)) is False
    assert window.pasted == []


def test_shift_still_picks(handler):
    """Shift 只影响大小写，不是命令修饰键。"""
    shortcut, window = handler

    assert shortcut.handle_key(_Event(Qt.Key.Key_V, Qt.KeyboardModifier.ShiftModifier)) is True
    assert window.pasted == [100 + 30]


def test_ctrl_f_still_reaches_the_search_box(handler):
    """这条在直选之前判定，不能被新的修饰键闸门挡掉。"""
    shortcut, window = handler

    assert shortcut.handle_key(_Event(Qt.Key.Key_F, Qt.KeyboardModifier.ControlModifier)) is True
    assert window.search_focused is True


# ── 可见 ≠ 有焦点 ──

class _Surface:
    def __init__(self, visible=True, active=True):
        self._visible = visible
        self._active = active

    def isVisible(self):
        return self._visible

    def isActiveWindow(self):
        return self._active


def test_handler_is_active_only_while_the_window_holds_focus():
    """窗口设成粘贴后常驻时会一直可见，但按键属于真正获焦的那个界面。

    分发器按优先级问一遍谁 active，剪贴板（60）排在钉图（40）前面，这里只看
    可见的话，钉图上的 Esc 会被剪贴板窗口截走。
    """
    assert ClipboardShortcutHandler(_Surface(visible=True, active=True)).is_active() is True
    assert ClipboardShortcutHandler(_Surface(visible=True, active=False)).is_active() is False
    assert ClipboardShortcutHandler(_Surface(visible=False, active=False)).is_active() is False


def test_handler_is_inactive_after_its_window_is_destroyed():
    class _Dead:
        def isVisible(self):
            raise RuntimeError("wrapped C/C++ object has been deleted")

    assert ClipboardShortcutHandler(_Dead()).is_active() is False
