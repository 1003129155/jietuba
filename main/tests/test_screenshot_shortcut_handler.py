# -*- coding: utf-8 -*-
"""
截图窗口快捷键分发状态机测试（ui/screenshot_window.py 的 ScreenshotShortcutHandler）

screenshot_window.py 有 625 条语句、覆盖率约 11%。其中 ScreenshotWindow 本身的
__init__ 会抓屏、建 Scene/View/Toolbar、注册全局快捷键单例、起 QTimer 并强制
show()，实例化它既慢又会污染 ShortcutManager 单例，因此本文件不构造窗口。

被测的 ScreenshotShortcutHandler 是纯 Python 类（基类 ShortcutHandler 不是
QObject），它是"按键 → 动作"的唯一入口：确认、钉图、撤销重做、删除、翻译、
取色、放大镜缩放、鼠标微移全都从这里分发。这段逻辑改错的后果是快捷键静默失效，
而现有测试完全不覆盖它。

隔离方式：用 __new__ 跳过 __init__（它会去读用户配置里的快捷键绑定），
手工装配 _bindings / _mouse_bindings / _move_keys / _window，用假事件对象和
MagicMock 窗口驱动。
"""
from types import SimpleNamespace
from unittest.mock import MagicMock

import pytest
from PySide6.QtCore import Qt, QRect, QRectF
from PySide6.QtWidgets import QApplication

from core import last_capture_region as region_module
from core.last_capture_region import set_last_region
from ui.screenshot_window import ScreenshotShortcutHandler

NO_MOD = Qt.KeyboardModifier.NoModifier
CTRL = Qt.KeyboardModifier.ControlModifier

# 刻意不用生产默认值，改用一组固定的测试绑定：
# 这样用例验证的是"分发逻辑"，而不是"用户当前配置恰好是什么"。
# confirm 特意避开 Enter，因为 Enter 另有一条固定分支，混在一起就分不清是哪条命中。
BINDINGS = {
    "inapp_confirm": (Qt.Key.Key_Space, NO_MOD),
    "inapp_pin": (Qt.Key.Key_P, CTRL),
    "inapp_undo": (Qt.Key.Key_Z, CTRL),
    "inapp_redo": (Qt.Key.Key_Y, CTRL),
    "inapp_delete": (Qt.Key.Key_Delete, NO_MOD),
    "inapp_translate": (Qt.Key.Key_T, CTRL),
    "inapp_zoom_in": (Qt.Key.Key_Plus, NO_MOD),
    "inapp_zoom_out": (Qt.Key.Key_Minus, NO_MOD),
    "inapp_restore_last_region": (Qt.Key.Key_L, NO_MOD),
}


class _FakeMouseEvent:
    """中键事件。刻意不提供 key() 和 isAutoRepeat()——handle_key 里若有哪条
    分支绕过 event_key()/event_is_auto_repeat() 直接取，就会在这里炸出来。"""

    def __init__(self, button=Qt.MouseButton.MiddleButton, modifiers=NO_MOD):
        self._button = button
        self._mods = modifiers

    def button(self):
        return self._button

    def modifiers(self):
        return self._mods


class _FakeKeyEvent:
    def __init__(self, key, modifiers=NO_MOD):
        self._key = key
        self._mods = modifiers

    def key(self):
        return self._key

    def modifiers(self):
        return self._mods


def _make_window(text_editing=False, confirmed=True, can_undo=True, can_redo=True,
                 magnifier=None, tool_id="cursor",
                 virtual_geometry=(0, 0, 1920, 1080)):
    """
    一个"什么都能被观测"的假截图窗口。

    注意 MagicMock 的任意属性都是真值，所以凡是被 if 判断的开关都必须显式赋值，
    否则用例会在错误的分支上通过。

    tool_id=None 模拟选区尚未确认、没有工具激活的状态；ToolController.current_tool_id
    把这种状态也归一成 "cursor"（见 tools/controller.py），和 tool_id="cursor" 是
    同一档语义（没有绘制工具在用），两者都应让"恢复上次选区"生效。
    """
    window = MagicMock()
    window._is_closing = False
    window.isVisible.return_value = True
    window._is_text_editing.return_value = text_editing
    window.scene.selection_model.is_confirmed = confirmed
    window.scene.undo_stack.canUndo.return_value = can_undo
    window.scene.undo_stack.canRedo.return_value = can_redo
    window.magnifier_overlay = magnifier
    window.scene.tool_controller.current_tool_id = tool_id or "cursor"
    window.virtual_x, window.virtual_y, window.virtual_width, window.virtual_height = (
        virtual_geometry
    )
    return window


def _make_magnifier(should_render=True, has_cursor=True, copy_ok=True):
    magnifier = MagicMock()
    magnifier.cursor_scene_pos = object() if has_cursor else None
    magnifier._should_render.return_value = should_render
    magnifier.copy_color_info.return_value = copy_ok
    return magnifier


class _FakeShiftEvent(_FakeKeyEvent):
    """Shift 键事件。补上 isAutoRepeat()，用来单独测自动重复被挡下的分支。"""

    def __init__(self, auto_repeat=False):
        super().__init__(Qt.Key.Key_Shift)
        self._auto_repeat = auto_repeat

    def isAutoRepeat(self):
        return self._auto_repeat


def _make_handler(window, move_keys=None, mouse_bindings=None):
    handler = ScreenshotShortcutHandler.__new__(ScreenshotShortcutHandler)
    handler._window = window
    handler._bindings = dict(BINDINGS)
    # 鼠标键绑定表。本文件只驱动键盘事件，但 _match 两张表都要查，缺了会 AttributeError
    handler._mouse_bindings = dict(mouse_bindings or {})
    handler._move_keys = dict(move_keys or {})
    # __init__ 被跳过，补上工具快捷键表（本文件只测动作/移动/放大镜分发）
    handler._tool_shortcuts = ()
    return handler


class TestHandlerIdentity:

    def test_priority_and_name_are_stable(self):
        handler = _make_handler(_make_window())
        assert handler.priority == 100
        assert handler.handler_name == "ScreenshotWindow"


class TestIsActive:

    def test_visible_and_not_closing_is_active(self):
        assert _make_handler(_make_window()).is_active() is True

    def test_missing_window_is_inactive(self):
        assert _make_handler(None).is_active() is False

    def test_closing_window_is_inactive(self):
        window = _make_window()
        window._is_closing = True
        assert _make_handler(window).is_active() is False

    def test_hidden_window_is_inactive(self):
        window = _make_window()
        window.isVisible.return_value = False
        assert _make_handler(window).is_active() is False

    def test_window_without_closing_flag_defaults_to_inactive(self):
        """getattr 的兜底值是 True（视为正在关闭），这是刻意的保守失败方向"""
        window = SimpleNamespace(isVisible=lambda: True)
        assert _make_handler(window).is_active() is False

    def test_modal_dialog_opened_over_the_screenshot_takes_the_keyboard(self, monkeypatch):
        """截图里打开的模态对话框（工具栏「调整」）要让出按键，否则在对话框里按 ESC 会结束截图"""
        monkeypatch.setattr(QApplication, "activeModalWidget", staticmethod(lambda: object()))
        assert _make_handler(_make_window()).is_active() is False


class TestEscape:

    def test_escape_closes_the_window_regardless_of_confirmation(self):
        for confirmed in (True, False):
            window = _make_window(confirmed=confirmed)
            handler = _make_handler(window)
            assert handler.handle_key(_FakeKeyEvent(Qt.Key.Key_Escape)) is True
            window.cleanup_and_close.assert_called_once()


class TestConfirmAndPin:

    def test_confirm_binding_triggers_action_when_selection_confirmed(self):
        window = _make_window(confirmed=True)
        handler = _make_handler(window)
        assert handler.handle_key(_FakeKeyEvent(Qt.Key.Key_Space)) is True
        window.action_handler.handle_confirm.assert_called_once()

    def test_confirm_binding_is_ignored_before_selection_is_confirmed(self):
        window = _make_window(confirmed=False)
        handler = _make_handler(window)
        assert handler.handle_key(_FakeKeyEvent(Qt.Key.Key_Space)) is False
        window.action_handler.handle_confirm.assert_not_called()

    def test_enter_confirms_even_though_it_is_not_a_configurable_binding(self):
        for key in (Qt.Key.Key_Return, Qt.Key.Key_Enter):
            window = _make_window(confirmed=True)
            handler = _make_handler(window)
            assert handler.handle_key(_FakeKeyEvent(key)) is True
            window.action_handler.handle_confirm.assert_called_once()

    def test_pin_binding_requires_a_confirmed_selection(self):
        confirmed_window = _make_window(confirmed=True)
        assert _make_handler(confirmed_window).handle_key(
            _FakeKeyEvent(Qt.Key.Key_P, CTRL)) is True
        confirmed_window.action_handler.handle_pin.assert_called_once()

        pending_window = _make_window(confirmed=False)
        assert _make_handler(pending_window).handle_key(
            _FakeKeyEvent(Qt.Key.Key_P, CTRL)) is False
        pending_window.action_handler.handle_pin.assert_not_called()

    def test_modifier_mismatch_does_not_trigger_a_binding(self):
        """绑定要求 Ctrl+P，裸 P 不能命中"""
        window = _make_window()
        handler = _make_handler(window)
        assert handler.handle_key(_FakeKeyEvent(Qt.Key.Key_P)) is False
        window.action_handler.handle_pin.assert_not_called()


class TestGlobalPinHotkey:
    """全局钉图热键被 RegisterHotKey 吃掉、到不了截图窗口，由 MainApp 转发到这里。"""

    def test_pins_the_confirmed_selection(self):
        window = _make_window(confirmed=True)
        _make_handler(window).pin_from_global_hotkey()
        window.action_handler.handle_pin.assert_called_once()

    def test_does_nothing_before_selection_is_confirmed(self):
        window = _make_window(confirmed=False)
        _make_handler(window).pin_from_global_hotkey()
        window.action_handler.handle_pin.assert_not_called()

    def test_still_pins_while_editing_text(self):
        window = _make_window(confirmed=True, text_editing=True)
        _make_handler(window).pin_from_global_hotkey()
        window.action_handler.handle_pin.assert_called_once()

    def test_does_nothing_while_window_is_closing(self):
        window = _make_window(confirmed=True)
        window._is_closing = True
        _make_handler(window).pin_from_global_hotkey()
        window.action_handler.handle_pin.assert_not_called()

    def test_main_app_forwards_to_an_active_screenshot_session(self, monkeypatch):
        from main_app import MainApp
        import clipboard.ui.windows.pin_window as pin_window_module
        pin_clipboard = MagicMock()
        monkeypatch.setattr(pin_window_module, "pin_latest_clipboard_image", pin_clipboard)

        app = SimpleNamespace(screenshot_window=MagicMock(_session_active=True))
        MainApp.pin_clipboard_image(app)

        app.screenshot_window.pin_from_global_hotkey.assert_called_once()
        pin_clipboard.assert_not_called()

    def test_main_app_pins_clipboard_outside_a_screenshot_session(self, monkeypatch):
        from main_app import MainApp
        import clipboard.ui.windows.pin_window as pin_window_module
        pin_clipboard = MagicMock()
        monkeypatch.setattr(pin_window_module, "pin_latest_clipboard_image", pin_clipboard)

        app = SimpleNamespace(screenshot_window=MagicMock(_session_active=False))
        MainApp.pin_clipboard_image(app)

        app.screenshot_window.pin_from_global_hotkey.assert_not_called()
        pin_clipboard.assert_called_once()


class TestUndoRedo:

    def test_undo_runs_when_the_stack_allows_it(self):
        window = _make_window(can_undo=True)
        assert _make_handler(window).handle_key(_FakeKeyEvent(Qt.Key.Key_Z, CTRL)) is True
        window.scene.undo_stack.undo.assert_called_once()

    def test_undo_key_is_still_consumed_when_the_stack_is_empty(self):
        """
        栈空时不执行撤销，但依然返回 True 把按键吃掉——
        否则 Ctrl+Z 会继续往下传给其它处理器。
        """
        window = _make_window(can_undo=False)
        assert _make_handler(window).handle_key(_FakeKeyEvent(Qt.Key.Key_Z, CTRL)) is True
        window.scene.undo_stack.undo.assert_not_called()

    def test_redo_runs_when_the_stack_allows_it(self):
        window = _make_window(can_redo=True)
        assert _make_handler(window).handle_key(_FakeKeyEvent(Qt.Key.Key_Y, CTRL)) is True
        window.scene.undo_stack.redo.assert_called_once()

    def test_redo_key_is_still_consumed_when_the_stack_is_empty(self):
        window = _make_window(can_redo=False)
        assert _make_handler(window).handle_key(_FakeKeyEvent(Qt.Key.Key_Y, CTRL)) is True
        window.scene.undo_stack.redo.assert_not_called()


class TestDelete:

    def test_delete_removes_the_selected_item(self):
        window = _make_window()
        assert _make_handler(window).handle_key(_FakeKeyEvent(Qt.Key.Key_Delete)) is True
        window.view.smart_edit_controller.delete_selected.assert_called_once()

    def test_delete_is_released_to_the_text_item_while_editing_text(self):
        """文字编辑中按 Delete 应该删字符，不能把整个图元删掉。

        回归用例：之前这里断言 handle_key 返回 True（事件被吃掉）——图元没被删对了，
        但事件也被吞了，文字框根本收不到这次按键，光标后面的字删不掉。必须返回
        False，让事件继续往下传给 QGraphicsTextItem 自己的按键处理。
        """
        window = _make_window(text_editing=True)
        assert _make_handler(window).handle_key(_FakeKeyEvent(Qt.Key.Key_Delete)) is False
        window.view.smart_edit_controller.delete_selected.assert_not_called()


class TestTranslate:

    def test_translate_binding_emits_the_toolbar_signal(self):
        window = _make_window(confirmed=True)
        assert _make_handler(window).handle_key(_FakeKeyEvent(Qt.Key.Key_T, CTRL)) is True
        window.toolbar.screenshot_translate_clicked.emit.assert_called_once()

    def test_translate_binding_is_ignored_before_selection_is_confirmed(self):
        window = _make_window(confirmed=False)
        assert _make_handler(window).handle_key(_FakeKeyEvent(Qt.Key.Key_T, CTRL)) is False
        window.toolbar.screenshot_translate_clicked.emit.assert_not_called()


class TestRestoreLastRegion:
    """按住恢复上次选区键（默认 L）时的分发逻辑，见 _restore_last_region。"""

    @pytest.fixture(autouse=True)
    def _reset_region(self):
        region_module._last_region = None
        yield
        region_module._last_region = None

    def test_restores_the_remembered_region_when_no_tool_is_active(self):
        set_last_region(QRect(100, 200, 300, 150))
        window = _make_window(confirmed=False, tool_id=None)
        assert _make_handler(window).handle_key(_FakeKeyEvent(Qt.Key.Key_L)) is True
        window.scene.selection_model.initialize_confirmed_rect.assert_called_once_with(
            QRectF(100, 200, 300, 150)
        )

    def test_region_on_a_monitor_left_of_the_primary_is_restored(self):
        """副屏在主屏左边时虚拟桌面原点是负的，选区坐标也是负的，不能判成越界。"""
        set_last_region(QRect(-1870, 50, 400, 300))
        window = _make_window(tool_id="cursor", virtual_geometry=(-1920, 0, 3840, 1080))
        assert _make_handler(window).handle_key(_FakeKeyEvent(Qt.Key.Key_L)) is True
        window.scene.selection_model.initialize_confirmed_rect.assert_called_once_with(
            QRectF(-1870, 50, 400, 300)
        )

    def test_does_nothing_while_a_drawing_tool_is_active(self):
        """已经选中绘制工具时不响应，避免覆盖正在进行的标注。"""
        set_last_region(QRect(0, 0, 300, 200))
        window = _make_window(tool_id="pen")
        assert _make_handler(window).handle_key(_FakeKeyEvent(Qt.Key.Key_L)) is False
        window.scene.selection_model.initialize_confirmed_rect.assert_not_called()

    def test_does_nothing_when_nothing_has_been_remembered_yet(self):
        window = _make_window(tool_id="cursor")
        assert _make_handler(window).handle_key(_FakeKeyEvent(Qt.Key.Key_L)) is False
        window.scene.selection_model.initialize_confirmed_rect.assert_not_called()

    def test_does_nothing_when_the_remembered_region_no_longer_fits_the_virtual_desktop(self):
        """典型场景：拔掉了显示器，上次选区落在当前虚拟桌面范围之外。"""
        set_last_region(QRect(3000, 0, 300, 200))
        window = _make_window(tool_id="cursor", virtual_geometry=(0, 0, 1920, 1080))
        assert _make_handler(window).handle_key(_FakeKeyEvent(Qt.Key.Key_L)) is False
        window.scene.selection_model.initialize_confirmed_rect.assert_not_called()

    def test_missing_scene_does_not_raise(self):
        window = _make_window(tool_id="cursor")
        window.scene = None
        assert _make_handler(window).handle_key(_FakeKeyEvent(Qt.Key.Key_L)) is False


class TestTextEditingPassthrough:

    def test_keys_that_belong_to_the_text_item_are_handed_back(self):
        cases = [
            (Qt.Key.Key_Return, NO_MOD),
            (Qt.Key.Key_Enter, NO_MOD),
            (Qt.Key.Key_C, NO_MOD),
            (Qt.Key.Key_D, NO_MOD),
            (Qt.Key.Key_Z, CTRL),
            (Qt.Key.Key_Y, CTRL),
        ]
        for key, mods in cases:
            window = _make_window(text_editing=True)
            handler = _make_handler(window)
            assert handler.handle_key(_FakeKeyEvent(key, mods)) is False, (key, mods)
            window.action_handler.handle_confirm.assert_not_called()
            window.scene.undo_stack.undo.assert_not_called()
            window.scene.undo_stack.redo.assert_not_called()

    def test_escape_still_closes_while_editing_text(self):
        window = _make_window(text_editing=True)
        assert _make_handler(window).handle_key(_FakeKeyEvent(Qt.Key.Key_Escape)) is True
        window.cleanup_and_close.assert_called_once()


class TestCursorNudge:

    def _patched_cursor(self, monkeypatch):
        moves = []

        class _FakeCursor:
            @staticmethod
            def pos():
                return SimpleNamespace(x=lambda: 100, y=lambda: 200)

            @staticmethod
            def setPos(x, y):
                moves.append((x, y))

        monkeypatch.setattr("PySide6.QtGui.QCursor", _FakeCursor)
        import PySide6.QtGui as qtgui
        # 打桩必须真的生效，否则用例会去动用户的真实鼠标指针
        assert qtgui.QCursor is _FakeCursor
        return moves

    def test_move_key_nudges_the_cursor_by_its_delta(self, monkeypatch):
        moves = self._patched_cursor(monkeypatch)
        handler = _make_handler(_make_window(), move_keys={Qt.Key.Key_Right: (1, 0)})
        assert handler.handle_key(_FakeKeyEvent(Qt.Key.Key_Right)) is True
        assert moves == [(101, 200)]

    def test_move_key_with_a_modifier_is_not_a_nudge(self, monkeypatch):
        moves = self._patched_cursor(monkeypatch)
        handler = _make_handler(_make_window(), move_keys={Qt.Key.Key_Right: (1, 0)})
        assert handler.handle_key(_FakeKeyEvent(Qt.Key.Key_Right, CTRL)) is False
        assert moves == []

    def test_move_keys_are_disabled_while_editing_text(self, monkeypatch):
        moves = self._patched_cursor(monkeypatch)
        handler = _make_handler(_make_window(text_editing=True),
                                move_keys={Qt.Key.Key_Right: (1, 0)})
        assert handler.handle_key(_FakeKeyEvent(Qt.Key.Key_Right)) is False
        assert moves == []


class TestMagnifier:

    def test_zoom_in_and_out_adjust_the_magnifier(self):
        for key, expected in ((Qt.Key.Key_Plus, 1), (Qt.Key.Key_Minus, -1)):
            magnifier = _make_magnifier()
            window = _make_window(magnifier=magnifier)
            assert _make_handler(window).handle_key(_FakeKeyEvent(key)) is True
            magnifier.adjust_zoom.assert_called_once_with(expected)

    def test_zoom_is_ignored_without_a_magnifier(self):
        window = _make_window(magnifier=None)
        assert _make_handler(window).handle_key(_FakeKeyEvent(Qt.Key.Key_Plus)) is False

    def test_zoom_is_ignored_when_the_magnifier_is_not_rendering(self):
        magnifier = _make_magnifier(should_render=False)
        window = _make_window(magnifier=magnifier)
        assert _make_handler(window).handle_key(_FakeKeyEvent(Qt.Key.Key_Plus)) is False
        magnifier.adjust_zoom.assert_not_called()

    def test_zoom_is_ignored_before_the_cursor_has_a_scene_position(self):
        magnifier = _make_magnifier(has_cursor=False)
        window = _make_window(magnifier=magnifier)
        assert _make_handler(window).handle_key(_FakeKeyEvent(Qt.Key.Key_Plus)) is False
        magnifier.adjust_zoom.assert_not_called()

    def test_bare_c_copies_the_colour_and_closes_the_window(self):
        magnifier = _make_magnifier(copy_ok=True)
        window = _make_window(magnifier=magnifier)
        assert _make_handler(window).handle_key(_FakeKeyEvent(Qt.Key.Key_C)) is True
        magnifier.copy_color_info.assert_called_once()
        window.cleanup_and_close.assert_called_once()

    def test_bare_c_keeps_the_window_open_when_copying_fails(self):
        magnifier = _make_magnifier(copy_ok=False)
        window = _make_window(magnifier=magnifier)
        assert _make_handler(window).handle_key(_FakeKeyEvent(Qt.Key.Key_C)) is False
        window.cleanup_and_close.assert_not_called()

    def test_bare_shift_cycles_the_colour_format(self):
        magnifier = _make_magnifier()
        window = _make_window(magnifier=magnifier)
        assert _make_handler(window).handle_key(_FakeShiftEvent()) is True
        magnifier.cycle_color_format.assert_called_once()

    def test_shift_auto_repeat_is_ignored(self):
        """按住 Shift 不放不该疯狂循环切换格式，只认第一次按下。"""
        magnifier = _make_magnifier()
        window = _make_window(magnifier=magnifier)
        assert _make_handler(window).handle_key(_FakeShiftEvent(auto_repeat=True)) is False
        magnifier.cycle_color_format.assert_not_called()

    def test_shift_is_ignored_when_the_magnifier_is_not_rendering(self):
        magnifier = _make_magnifier(should_render=False)
        window = _make_window(magnifier=magnifier)
        assert _make_handler(window).handle_key(_FakeShiftEvent()) is False
        magnifier.cycle_color_format.assert_not_called()

    def test_shift_is_ignored_without_a_magnifier(self):
        window = _make_window(magnifier=None)
        assert _make_handler(window).handle_key(_FakeShiftEvent()) is False


class TestUnhandledKeys:

    def test_unbound_key_is_passed_on(self):
        window = _make_window()
        assert _make_handler(window).handle_key(_FakeKeyEvent(Qt.Key.Key_F7)) is False

    def test_binding_absent_from_the_config_never_matches(self):
        window = _make_window()
        handler = _make_handler(window)
        handler._bindings = {}
        assert handler.handle_key(_FakeKeyEvent(Qt.Key.Key_Space)) is False
        window.action_handler.handle_confirm.assert_not_called()


class TestMiddleClickBinding:
    """绑成中键的动作要和绑成键盘时走同一条 if 链。"""

    def test_middle_click_triggers_the_bound_action(self):
        window = _make_window(confirmed=True)
        handler = _make_handler(window, mouse_bindings={
            "inapp_confirm": (Qt.MouseButton.MiddleButton, NO_MOD)})
        assert handler.handle_mouse(_FakeMouseEvent()) is True
        window.action_handler.handle_confirm.assert_called_once()

    def test_middle_click_respects_the_same_guards_as_the_keyboard(self):
        """确认动作在选区未确认时不该触发，鼠标这条路也一样。"""
        window = _make_window(confirmed=False)
        handler = _make_handler(window, mouse_bindings={
            "inapp_confirm": (Qt.MouseButton.MiddleButton, NO_MOD)})
        assert handler.handle_mouse(_FakeMouseEvent()) is False
        window.action_handler.handle_confirm.assert_not_called()

    def test_an_unbound_middle_click_trips_no_key_only_branch(self):
        """ESC、Enter、取色 C 都是硬编码的键专属分支，必须对鼠标事件全部落空。

        它们靠 event_key() 返回 Key_unknown 落空；谁把那里改回 event.key()，
        这条会直接 AttributeError。
        """
        window = _make_window(confirmed=True)
        handler = _make_handler(window)
        assert handler.handle_mouse(_FakeMouseEvent()) is False
        window.cleanup_and_close.assert_not_called()
        window.action_handler.handle_confirm.assert_not_called()

    def test_other_buttons_are_not_the_middle_binding(self):
        window = _make_window(confirmed=True)
        handler = _make_handler(window, mouse_bindings={
            "inapp_confirm": (Qt.MouseButton.MiddleButton, NO_MOD)})
        left = _FakeMouseEvent(button=Qt.MouseButton.LeftButton)
        assert handler.handle_mouse(left) is False
        window.action_handler.handle_confirm.assert_not_called()


class TestWindowForwardsGlobalPin:
    def test_window_hands_the_global_pin_to_its_shortcut_handler(self):
        from ui.screenshot_window import ScreenshotWindow

        handler = MagicMock()
        ScreenshotWindow.pin_from_global_hotkey(SimpleNamespace(_shortcut_handler=handler))
        ScreenshotWindow.pin_from_global_hotkey(SimpleNamespace(_shortcut_handler=None))

        handler.pin_from_global_hotkey.assert_called_once_with()
