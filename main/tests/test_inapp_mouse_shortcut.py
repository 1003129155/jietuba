# -*- coding: utf-8 -*-
"""应用内快捷键绑定鼠标中键。

这条路和全局侧键是两套东西，最要紧的是它们不能互相串：中键绝不能被全局热键那套
接管——那意味着挂低级钩子、全局抑制中键，所有程序的粘贴/关标签页/自动滚动都会
失效。下面第一组就钉这件事。
"""
import pytest
from PySide6.QtCore import QEvent, QPointF, Qt
from PySide6.QtGui import QKeyEvent, QMouseEvent

from core.shortcut_manager import (
    MOUSE_BUTTON_BACK, MOUSE_BUTTON_MIDDLE, ShortcutHandler, ShortcutManager,
    event_is_auto_repeat, event_key, inapp_shortcut_display_text,
    is_inapp_mouse_shortcut, is_mouse_button_hotkey, is_reserved_inapp_shortcut,
    load_inapp_bindings, load_inapp_mouse_bindings, match_inapp_binding,
    parse_inapp_mouse_to_qt, parse_shortcut_to_qt,
)


def _mouse(button=Qt.MouseButton.MiddleButton,
           mods=Qt.KeyboardModifier.NoModifier,
           etype=QEvent.Type.MouseButtonPress):
    return QMouseEvent(etype, QPointF(0, 0), QPointF(0, 0),
                       button, button, mods)


def _key(key=Qt.Key.Key_C, mods=Qt.KeyboardModifier.NoModifier):
    return QKeyEvent(QEvent.Type.KeyPress, key, mods)


# ============================================================================
# 中键不得进入全局热键那条路
# ============================================================================

def test_middle_button_is_not_a_global_mouse_hotkey():
    """全局路由认了中键，就等于全局抑制中键——这正是没做全局中键的原因。"""
    assert is_mouse_button_hotkey(MOUSE_BUTTON_MIDDLE) is False


def test_global_side_buttons_are_not_inapp_mouse_shortcuts():
    """反向也要隔开：侧键在录入期间被钩子整个吞掉，Qt 收不到，这条路走不通。"""
    assert is_inapp_mouse_shortcut(MOUSE_BUTTON_BACK) is False


# ============================================================================
# 解析：一个配置项要么键盘要么鼠标，两边互不相认
# ============================================================================

@pytest.mark.parametrize("text,mods", [
    (MOUSE_BUTTON_MIDDLE, Qt.KeyboardModifier.NoModifier),
    ("ctrl+mousemiddle", Qt.KeyboardModifier.ControlModifier),
    ("Shift+MouseMiddle", Qt.KeyboardModifier.ShiftModifier),
])
def test_mouse_binding_parses_with_modifiers(text, mods):
    assert parse_inapp_mouse_to_qt(text) == (Qt.MouseButton.MiddleButton, mods)


@pytest.mark.parametrize("text", ["ctrl+c", "pageup", "f5", "", None, "ctrl+"])
def test_keyboard_text_is_not_a_mouse_binding(text):
    assert parse_inapp_mouse_to_qt(text) is None


@pytest.mark.parametrize("text", [MOUSE_BUTTON_MIDDLE, "ctrl+mousemiddle"])
def test_keyboard_parser_rejects_mouse_tokens(text):
    """键盘解析必须放行鼠标 token，否则 load_inapp_bindings 会把它当键收进去。"""
    assert parse_shortcut_to_qt(text) is None


def test_keyboard_parsing_is_unchanged():
    """改 _split_modifiers 时顺手回归一下键盘那侧。"""
    assert parse_shortcut_to_qt("ctrl+c") == (
        Qt.Key.Key_C, Qt.KeyboardModifier.ControlModifier)
    assert parse_shortcut_to_qt("shift+pageup") == (
        Qt.Key.Key_PageUp, Qt.KeyboardModifier.ShiftModifier)
    assert parse_shortcut_to_qt("f5") == (
        Qt.Key.Key_F5, Qt.KeyboardModifier.NoModifier)


def test_middle_button_is_not_reserved():
    """保留键只有 ESC；中键被当成保留键就会在保存时被清空。"""
    assert is_reserved_inapp_shortcut(MOUSE_BUTTON_MIDDLE) is False


# ============================================================================
# 两张绑定表天然互斥
# ============================================================================

def test_the_two_binding_tables_never_hold_the_same_key():
    from settings import get_tool_settings_manager

    cfg = get_tool_settings_manager()
    cfg.set_inapp_shortcut("inapp_delete", MOUSE_BUTTON_MIDDLE)
    cfg.set_inapp_shortcut("inapp_undo", "ctrl+z")
    keys = ["inapp_delete", "inapp_undo"]

    keyboard = load_inapp_bindings(keys)
    mouse = load_inapp_mouse_bindings(keys)

    assert "inapp_delete" in mouse and "inapp_delete" not in keyboard
    assert "inapp_undo" in keyboard and "inapp_undo" not in mouse


# ============================================================================
# 匹配：事件类型决定查哪张表，不能跨类型误中
# ============================================================================

def test_mouse_event_matches_only_the_mouse_table():
    keyboard = {"a": (Qt.Key.Key_C, Qt.KeyboardModifier.NoModifier)}
    mouse = {"a": (Qt.MouseButton.MiddleButton, Qt.KeyboardModifier.NoModifier)}
    assert match_inapp_binding(_mouse(), "a", keyboard, mouse) is True
    assert match_inapp_binding(_mouse(), "a", keyboard, {}) is False


def test_key_event_matches_only_the_keyboard_table():
    keyboard = {"a": (Qt.Key.Key_C, Qt.KeyboardModifier.NoModifier)}
    mouse = {"a": (Qt.MouseButton.MiddleButton, Qt.KeyboardModifier.NoModifier)}
    assert match_inapp_binding(_key(), "a", keyboard, mouse) is True
    assert match_inapp_binding(_key(), "a", {}, mouse) is False


def test_modifiers_must_match_on_mouse_bindings():
    mouse = {"a": (Qt.MouseButton.MiddleButton,
                   Qt.KeyboardModifier.ControlModifier)}
    assert match_inapp_binding(_mouse(), "a", {}, mouse) is False
    ctrl_click = _mouse(mods=Qt.KeyboardModifier.ControlModifier)
    assert match_inapp_binding(ctrl_click, "a", {}, mouse) is True


def test_other_buttons_never_match():
    mouse = {"a": (Qt.MouseButton.MiddleButton, Qt.KeyboardModifier.NoModifier)}
    left = _mouse(button=Qt.MouseButton.LeftButton)
    assert match_inapp_binding(left, "a", {}, mouse) is False


# ============================================================================
# handle_key 同时跑两种事件所依赖的取值
# ============================================================================

def test_event_key_is_unknown_for_mouse_events():
    """键专属分支（ESC、微移键、取色 C）靠这个对鼠标事件落空。"""
    assert event_key(_mouse()) == Qt.Key.Key_unknown
    assert event_key(_key(Qt.Key.Key_Escape)) == Qt.Key.Key_Escape


def test_auto_repeat_is_false_for_mouse_events():
    """鼠标事件没有 isAutoRepeat()，直接调用会 AttributeError。"""
    assert event_is_auto_repeat(_mouse()) is False


# ============================================================================
# 显示文案
# ============================================================================

@pytest.mark.parametrize("text,expected", [
    ("ctrl+c", "CTRL+C"),
    ("r", "R"),
    ("", ""),
])
def test_keyboard_display_is_unchanged(text, expected):
    assert inapp_shortcut_display_text(text) == expected


def test_mouse_display_is_not_the_raw_token():
    """右键菜单里不该出现 MOUSEMIDDLE。"""
    assert "MOUSEMIDDLE" not in inapp_shortcut_display_text(MOUSE_BUTTON_MIDDLE)
    assert inapp_shortcut_display_text("ctrl+mousemiddle").startswith("CTRL+")


# ============================================================================
# 分发：装在 QApplication 上，每一次点击都要过，所以先看它挡不挡得住
# ============================================================================

class _Handler:
    def __init__(self, active=True, consume=True):
        self._active = active
        self._consume = consume
        self.seen = []

    def is_active(self):
        return self._active

    def handle_mouse(self, event):
        self.seen.append(event.button())
        return self._consume

    def handle_key(self, event):
        return False

    @property
    def priority(self):
        return 10

    @property
    def handler_name(self):
        return "Fake"


def _manager(handler):
    manager = ShortcutManager()
    manager._handlers = [handler]
    return manager


def test_non_middle_buttons_are_not_dispatched(qapp):
    """左右键点击是应用里最高频的事件，绝不能进 handler 链。"""
    handler = _Handler()
    manager = _manager(handler)
    for button in (Qt.MouseButton.LeftButton, Qt.MouseButton.RightButton):
        assert manager._filter_inapp_mouse(None, _mouse(button=button)) is False
    assert handler.seen == []


def test_middle_press_reaches_the_handler_chain(qapp):
    handler = _Handler()
    manager = _manager(handler)
    assert manager._filter_inapp_mouse(None, _mouse()) is True
    assert handler.seen == [Qt.MouseButton.MiddleButton]


def test_release_is_swallowed_together_with_a_consumed_press(qapp):
    """只吞按下会让控件收到一个没有配对按下的抬起，行为未定义。"""
    manager = _manager(_Handler(consume=True))
    manager._filter_inapp_mouse(None, _mouse())
    release = _mouse(etype=QEvent.Type.MouseButtonRelease)
    assert manager._filter_inapp_mouse(None, release) is True


def test_release_passes_through_when_the_press_was_not_consumed(qapp):
    manager = _manager(_Handler(consume=False))
    manager._filter_inapp_mouse(None, _mouse())
    release = _mouse(etype=QEvent.Type.MouseButtonRelease)
    assert manager._filter_inapp_mouse(None, release) is False


def test_the_recorder_widget_gets_the_click_before_any_handler(qapp):
    """录入框标了记号就必须放行。

    钉图 handler 的 is_active() 只看指针位置、不看焦点，不挡住的话设置页上
    这一下会被钉图先吃掉，永远录不到。
    """
    handler = _Handler()
    manager = _manager(handler)

    class _Recorder:
        _captures_inapp_mouse_shortcut = True

    assert manager._filter_inapp_mouse(_Recorder(), _mouse()) is False
    assert handler.seen == []


# ============================================================================
# 录入框
# ============================================================================

def test_recorder_records_middle_click(qapp):
    from ui.inapp_key_edit import InAppKeyEdit

    edit = InAppKeyEdit()
    edit.mousePressEvent(_mouse())
    assert edit.text() == MOUSE_BUTTON_MIDDLE


def test_recorder_records_modifiers_with_middle_click(qapp):
    from ui.inapp_key_edit import InAppKeyEdit

    edit = InAppKeyEdit()
    edit.mousePressEvent(_mouse(mods=Qt.KeyboardModifier.ControlModifier))
    assert edit.text() == "ctrl+" + MOUSE_BUTTON_MIDDLE


def test_recorder_text_round_trips_through_the_parser(qapp):
    """录入框写进配置的就是这段文本，解析不回来就是绑了个存不上的键。"""
    from ui.inapp_key_edit import InAppKeyEdit

    edit = InAppKeyEdit()
    edit.mousePressEvent(_mouse(mods=Qt.KeyboardModifier.ControlModifier))
    assert parse_inapp_mouse_to_qt(edit.text()) == (
        Qt.MouseButton.MiddleButton, Qt.KeyboardModifier.ControlModifier)


def test_recorder_ignores_other_buttons(qapp):
    """左键要留给获取焦点，右键菜单已被禁掉。"""
    from ui.inapp_key_edit import InAppKeyEdit

    edit = InAppKeyEdit()
    edit.setText("ctrl+c")
    edit.mousePressEvent(_mouse(button=Qt.MouseButton.LeftButton))
    assert edit.text() == "ctrl+c"


# ============================================================================
# 三个消费方都接上了
# ============================================================================

def test_every_inapp_handler_supports_mouse_bindings():
    """新增 handler 时容易只实现 handle_key——那样中键在那个窗口里静默失效。"""
    from gif.drawing_view import GifDrawingShortcutHandler
    from pin.pin_shortcut import _PinHandlerBase
    from ui.screenshot_window import ScreenshotShortcutHandler

    for cls in (GifDrawingShortcutHandler, _PinHandlerBase,
                ScreenshotShortcutHandler):
        assert cls.handle_mouse is not ShortcutHandler.handle_mouse, cls.__name__


# ============================================================================
# eventFilter 的改动不能影响原有的键盘分发
# ============================================================================

class _KeyHandler(_Handler):
    def __init__(self):
        super().__init__()
        self.keys = []

    def handle_key(self, event):
        self.keys.append(event.key())
        return True


def test_key_events_still_reach_handle_key(qapp):
    """加鼠标分支时动了 eventFilter 的开头，键盘那条路必须原样。"""
    handler = _KeyHandler()
    manager = _manager(handler)
    assert manager.eventFilter(None, _key(Qt.Key.Key_F5)) is True
    assert handler.keys == [Qt.Key.Key_F5]
    assert handler.seen == []


def test_unrelated_event_types_are_ignored(qapp):
    """过滤器装在 QApplication 上，除按键和中键外一律原样放行。"""
    handler = _KeyHandler()
    manager = _manager(handler)
    assert manager.eventFilter(None, QEvent(QEvent.Type.Paint)) is False
    assert manager.eventFilter(None, QEvent(QEvent.Type.MouseMove)) is False
    assert handler.keys == [] and handler.seen == []


def test_mouse_move_never_enters_the_handler_chain(qapp):
    """鼠标移动是应用里最高频的事件，进 handler 链会让整个界面变卡。"""
    handler = _Handler()
    manager = _manager(handler)
    assert QEvent.Type.MouseMove not in ShortcutManager._INAPP_MOUSE_EVENT_TYPES
    assert manager.eventFilter(None, QEvent(QEvent.Type.MouseMove)) is False
    assert handler.seen == []
