# -*- coding: utf-8 -*-
"""
钉图快捷键行为测试

pin_shortcut.py 决定了鼠标悬停在钉图上时按键会发生什么，此前完全没有测试。
它不需要真实窗口就能测——两个 Handler 只通过鸭子类型访问钉图对象的属性，
所以这里用假钉图驱动，覆盖真正要紧的行为：

- 编辑态与普通态的分工：同一个按键在两种状态下必须落到不同的动作上。
  最典型的是 ESC——普通态关闭钉图，编辑态只退出编辑。如果编辑态的
  Handler 没接住 ESC，用户画到一半按 ESC 会直接把钉图连同未保存的
  标注一起关掉。
- 复制键优先复制 OCR 选中的文字，没有选区时才复制整张图。
- 未识别的按键必须返回 False 向下传递，否则会吞掉别的模块的快捷键。
- 鼠标下方钉图的查找：上层优先，且要跳过已销毁的窗口。
"""
import pytest
from PySide6.QtCore import Qt, QRect, QPoint
from PySide6.QtGui import QKeyEvent, QCursor
from PySide6.QtWidgets import QApplication


@pytest.fixture(scope="module")
def qapp():
    app = QApplication.instance()
    if app is None:
        app = QApplication([])
    yield app


# ============================================================================
# 测试替身
# ============================================================================

class FakeOCRLayer:
    def __init__(self, selected_text="", active=True):
        self._selected_text = selected_text
        self._active_flag = active
        # 与 OCRTextLayer 保持同一类型：Optional[Tuple[item_index, char_index]]。
        # 这里必须是元组而不是 QPoint——源码用 `if selection_start or ...` 判断
        # 有无选区，而 PySide6 的 QPoint(0, 0) 布尔值为 False，用错类型会让
        # 测试对着一个现实中不存在的行为报警。
        self.selection_start = None
        self.selection_end = None
        self.copied = 0
        self.cleared = 0

    def get_selected_text(self):
        return self._selected_text

    def _copy_selected_text(self):
        self.copied += 1

    def _is_active(self):
        return self._active_flag

    def clear_selection(self):
        self.cleared += 1


class FakeCanvas:
    def __init__(self, is_editing=False):
        self.is_editing = is_editing
        self.deactivated = 0

    def deactivate_tool(self):
        self.deactivated += 1


class FakePin:
    """只实现 Handler 会用到的那部分钉图接口"""

    def __init__(self, rect=QRect(0, 0, 100, 100), editing=False, ocr_layer=None):
        self._rect = rect
        self.canvas = FakeCanvas(editing)
        self.ocr_text_layer = ocr_layer
        self.toolbar = None
        self.view = None
        self._is_closed = False

        self.copied = 0
        self.closed = 0
        self.thumbnail_toggled = 0
        self.toolbar_toggled = 0

    def geometry(self):
        return self._rect

    def isVisible(self):
        return True

    def copy_to_clipboard(self):
        self.copied += 1

    def close_window(self):
        self.closed += 1

    def toggle_thumbnail_mode(self):
        self.thumbnail_toggled += 1

    def toggle_toolbar(self):
        self.toolbar_toggled += 1


class FakeController:
    """替代 PinShortcutController，由测试直接指定鼠标下方是哪个钉图"""

    def __init__(self, pin=None):
        self.pin = pin

    def _find_pin_under_cursor(self):
        return self.pin


def _key_event(key, modifiers=Qt.KeyboardModifier.NoModifier):
    return QKeyEvent(QKeyEvent.Type.KeyPress, key, modifiers)


@pytest.fixture
def edit_handler(qapp):
    from pin.pin_shortcut import PinEditShortcutHandler
    return PinEditShortcutHandler(FakeController())


@pytest.fixture
def normal_handler(qapp):
    from pin.pin_shortcut import PinNormalShortcutHandler
    return PinNormalShortcutHandler(FakeController())


def _bind(handler, cfg_key, key, mods=Qt.KeyboardModifier.NoModifier):
    """直接指定某个配置项对应的按键，避免测试依赖用户的本地配置"""
    handler._bindings[cfg_key] = (key, mods)


# ============================================================================
# 激活条件
# ============================================================================

class TestHandlerActivation:
    """
    两个 Handler 的激活条件必须互斥：同一时刻只能有一个接管按键，
    否则一次按键会被处理两遍。
    """

    def test_neither_is_active_without_a_pin_under_the_cursor(self, edit_handler, normal_handler):
        assert edit_handler.is_active() is False
        assert normal_handler.is_active() is False

    def test_edit_handler_activates_only_while_editing(self, edit_handler):
        edit_handler._controller.pin = FakePin(editing=True)
        assert edit_handler.is_active() is True

        edit_handler._controller.pin = FakePin(editing=False)
        assert edit_handler.is_active() is False

    def test_normal_handler_activates_only_outside_editing(self, normal_handler):
        normal_handler._controller.pin = FakePin(editing=False)
        assert normal_handler.is_active() is True

        normal_handler._controller.pin = FakePin(editing=True)
        assert normal_handler.is_active() is False

    @pytest.mark.parametrize("editing", [True, False])
    def test_exactly_one_handler_is_active_at_a_time(self, edit_handler, normal_handler, editing):
        pin = FakePin(editing=editing)
        edit_handler._controller.pin = pin
        normal_handler._controller.pin = pin

        assert [edit_handler.is_active(), normal_handler.is_active()].count(True) == 1

    def test_edit_handler_has_higher_priority_than_normal(self, edit_handler, normal_handler):
        """编辑态的处理器必须排在前面，否则编辑中的按键会被普通态先吃掉"""
        assert edit_handler.priority > normal_handler.priority


# ============================================================================
# 普通模式
# ============================================================================

class TestNormalModeKeys:
    """非编辑状态下的按键"""

    def test_escape_closes_the_pin(self, normal_handler):
        pin = FakePin(editing=False)
        normal_handler._controller.pin = pin

        assert normal_handler.handle_key(_key_event(Qt.Key.Key_Escape)) is True
        assert pin.closed == 1

    def test_copy_key_copies_the_image(self, normal_handler):
        pin = FakePin(editing=False)
        normal_handler._controller.pin = pin
        _bind(normal_handler, "inapp_copy_pin", Qt.Key.Key_C,
              Qt.KeyboardModifier.ControlModifier)

        event = _key_event(Qt.Key.Key_C, Qt.KeyboardModifier.ControlModifier)
        assert normal_handler.handle_key(event) is True
        assert pin.copied == 1

    def test_copy_prefers_selected_ocr_text_over_the_image(self, normal_handler):
        """OCR 里选了文字就复制文字，而不是把整张图塞进剪贴板"""
        ocr = FakeOCRLayer(selected_text="hello")
        pin = FakePin(editing=False, ocr_layer=ocr)
        normal_handler._controller.pin = pin
        _bind(normal_handler, "inapp_copy_pin", Qt.Key.Key_C,
              Qt.KeyboardModifier.ControlModifier)

        normal_handler.handle_key(_key_event(Qt.Key.Key_C, Qt.KeyboardModifier.ControlModifier))

        assert ocr.copied == 1
        assert pin.copied == 0

    def test_empty_ocr_selection_falls_back_to_copying_the_image(self, normal_handler):
        ocr = FakeOCRLayer(selected_text="")
        pin = FakePin(editing=False, ocr_layer=ocr)
        normal_handler._controller.pin = pin
        _bind(normal_handler, "inapp_copy_pin", Qt.Key.Key_C,
              Qt.KeyboardModifier.ControlModifier)

        normal_handler.handle_key(_key_event(Qt.Key.Key_C, Qt.KeyboardModifier.ControlModifier))

        assert ocr.copied == 0
        assert pin.copied == 1

    def test_thumbnail_key_toggles_thumbnail_mode(self, normal_handler):
        pin = FakePin(editing=False)
        normal_handler._controller.pin = pin
        _bind(normal_handler, "inapp_thumbnail", Qt.Key.Key_R)

        assert normal_handler.handle_key(_key_event(Qt.Key.Key_R)) is True
        assert pin.thumbnail_toggled == 1

    def test_unknown_key_is_passed_through(self, normal_handler):
        """不认识的按键必须放行，否则会吞掉其它模块的快捷键"""
        normal_handler._controller.pin = FakePin(editing=False)
        assert normal_handler.handle_key(_key_event(Qt.Key.Key_F5)) is False

    def test_modifier_must_match_exactly(self, normal_handler):
        """配置的是 Ctrl+C，那么不带修饰键的 C 不应触发复制"""
        pin = FakePin(editing=False)
        normal_handler._controller.pin = pin
        _bind(normal_handler, "inapp_copy_pin", Qt.Key.Key_C,
              Qt.KeyboardModifier.ControlModifier)

        assert normal_handler.handle_key(_key_event(Qt.Key.Key_C)) is False
        assert pin.copied == 0

    def test_nothing_happens_without_a_pin_under_the_cursor(self, normal_handler):
        assert normal_handler.handle_key(_key_event(Qt.Key.Key_Escape)) is False

    def test_keys_are_ignored_while_editing(self, normal_handler):
        """编辑态由另一个 Handler 负责，这里必须放行"""
        pin = FakePin(editing=True)
        normal_handler._controller.pin = pin

        assert normal_handler.handle_key(_key_event(Qt.Key.Key_Escape)) is False
        assert pin.closed == 0


# ============================================================================
# 编辑模式
# ============================================================================

class TestEditModeKeys:
    """编辑状态下的按键"""

    def test_escape_exits_editing_instead_of_closing_the_pin(self, edit_handler):
        """
        最要紧的一条：编辑时按 ESC 只退出编辑模式，
        绝不能像普通模式那样把钉图连同未保存的标注一起关掉。
        """
        pin = FakePin(editing=True)
        edit_handler._controller.pin = pin

        assert edit_handler.handle_key(_key_event(Qt.Key.Key_Escape)) is True
        assert pin.canvas.deactivated == 1
        assert pin.closed == 0

    def test_escape_first_clears_an_ocr_selection(self, edit_handler):
        """有 OCR 选区时，第一次 ESC 先清选区，不急着退出编辑"""
        ocr = FakeOCRLayer(selected_text="hello")
        ocr.selection_start = (0, 0)      # 第 0 个文本项的第 0 个字符
        ocr.selection_end = (0, 5)
        pin = FakePin(editing=True, ocr_layer=ocr)
        edit_handler._controller.pin = pin

        assert edit_handler.handle_key(_key_event(Qt.Key.Key_Escape)) is True
        assert ocr.cleared == 1
        assert pin.canvas.deactivated == 0

    def test_second_escape_then_exits_editing(self, edit_handler):
        ocr = FakeOCRLayer(selected_text="")
        pin = FakePin(editing=True, ocr_layer=ocr)
        edit_handler._controller.pin = pin

        edit_handler.handle_key(_key_event(Qt.Key.Key_Escape))

        assert pin.canvas.deactivated == 1
        assert pin.closed == 0

    def test_copy_prefers_selected_ocr_text(self, edit_handler):
        ocr = FakeOCRLayer(selected_text="hello")
        pin = FakePin(editing=True, ocr_layer=ocr)
        edit_handler._controller.pin = pin
        _bind(edit_handler, "inapp_copy_pin", Qt.Key.Key_C,
              Qt.KeyboardModifier.ControlModifier)

        edit_handler.handle_key(_key_event(Qt.Key.Key_C, Qt.KeyboardModifier.ControlModifier))

        assert ocr.copied == 1
        assert pin.copied == 0

    def test_unknown_key_is_passed_through(self, edit_handler):
        """编辑态下的普通字符必须放行，否则打字会被吞掉"""
        edit_handler._controller.pin = FakePin(editing=True)
        assert edit_handler.handle_key(_key_event(Qt.Key.Key_A)) is False

    def test_keys_are_ignored_when_not_editing(self, edit_handler):
        pin = FakePin(editing=False)
        edit_handler._controller.pin = pin

        assert edit_handler.handle_key(_key_event(Qt.Key.Key_Escape)) is False
        assert pin.canvas.deactivated == 0


# ============================================================================
# 鼠标下方钉图的查找
# ============================================================================

class TestFindPinUnderCursor:
    """PinShortcutController._find_pin_under_cursor"""

    @pytest.fixture
    def controller(self, qapp):
        from pin.pin_shortcut import PinShortcutController
        ctrl = PinShortcutController.instance()
        ctrl._pin_windows = []       # 隔离全局单例的既有状态
        yield ctrl
        ctrl._pin_windows = []

    def test_returns_none_when_no_pin_is_under_the_cursor(self, controller, monkeypatch):
        monkeypatch.setattr(QCursor, "pos", staticmethod(lambda: QPoint(500, 500)))
        controller.register(FakePin(rect=QRect(0, 0, 100, 100)))

        assert controller._find_pin_under_cursor() is None

    def test_finds_the_pin_containing_the_cursor(self, controller, monkeypatch):
        monkeypatch.setattr(QCursor, "pos", staticmethod(lambda: QPoint(50, 50)))
        pin = FakePin(rect=QRect(0, 0, 100, 100))
        controller.register(pin)

        assert controller._find_pin_under_cursor() is pin

    def test_topmost_pin_wins_when_they_overlap(self, controller, monkeypatch):
        """后创建的钉图在上层，重叠时应命中它"""
        monkeypatch.setattr(QCursor, "pos", staticmethod(lambda: QPoint(50, 50)))
        below = FakePin(rect=QRect(0, 0, 100, 100))
        above = FakePin(rect=QRect(0, 0, 100, 100))
        controller.register(below)
        controller.register(above)

        assert controller._find_pin_under_cursor() is above

    def test_closed_pins_are_skipped_and_pruned(self, controller, monkeypatch):
        """已关闭的窗口不能再接收按键，也应从列表里清掉"""
        monkeypatch.setattr(QCursor, "pos", staticmethod(lambda: QPoint(50, 50)))
        dead = FakePin(rect=QRect(0, 0, 100, 100))
        dead._is_closed = True
        alive = FakePin(rect=QRect(0, 0, 100, 100))
        controller.register(dead)
        controller.register(alive)

        assert controller._find_pin_under_cursor() is alive
        assert dead not in controller._pin_windows

    def test_register_is_idempotent(self, controller):
        pin = FakePin()
        controller.register(pin)
        controller.register(pin)
        assert controller._pin_windows.count(pin) == 1

    def test_unregister_removes_the_pin(self, controller):
        pin = FakePin()
        controller.register(pin)
        controller.unregister(pin)
        assert pin not in controller._pin_windows

    def test_unregistering_an_unknown_pin_is_harmless(self, controller):
        controller.unregister(FakePin())      # 不应抛异常

    def test_is_alive_reports_false_for_a_destroyed_window(self, controller):
        class Destroyed:
            def isVisible(self):
                raise RuntimeError("wrapped C/C++ object has been deleted")

        assert controller._is_alive(Destroyed()) is False


# ============================================================================
# 智能翻译热键拦截
# ============================================================================

class TestTranslationHotkeyInterception:
    """
    钉图不抢键盘焦点，智能翻译注入的复制热键会落到别的窗口上，
    所以这里要在热键阶段直接把 OCR 选区文本交给翻译。
    """

    class FakeTranslationController:
        def __init__(self):
            self.translated = []

        def translate_selection(self, text):
            self.translated.append(text)

    def _callback_of(self, owner):
        return owner.translate_selection

    def test_selected_text_is_sent_straight_to_translation(self, normal_handler):
        owner = self.FakeTranslationController()
        type(owner).__name__ = "SmartTranslationController"
        ocr = FakeOCRLayer(selected_text="  待翻译  ")
        normal_handler._controller.pin = FakePin(ocr_layer=ocr)

        handled = normal_handler.handle_hotkey(1, self._callback_of(owner))

        assert handled is True
        assert owner.translated == ["待翻译"]      # 前后空白应被去掉

    def test_without_a_selection_it_falls_back_to_the_normal_flow(self, normal_handler):
        owner = self.FakeTranslationController()
        type(owner).__name__ = "SmartTranslationController"
        normal_handler._controller.pin = FakePin(ocr_layer=FakeOCRLayer(selected_text=""))

        assert normal_handler.handle_hotkey(1, self._callback_of(owner)) is False
        assert owner.translated == []

    def test_other_callbacks_are_not_intercepted(self, normal_handler):
        """只拦截智能翻译，别的热键回调必须原样放行"""
        class SomethingElse:
            def run(self):
                pass

        other = SomethingElse()
        normal_handler._controller.pin = FakePin(ocr_layer=FakeOCRLayer(selected_text="x"))

        assert normal_handler.handle_hotkey(1, other.run) is False

    def test_plain_function_callback_is_not_intercepted(self, normal_handler):
        normal_handler._controller.pin = FakePin(ocr_layer=FakeOCRLayer(selected_text="x"))
        assert normal_handler.handle_hotkey(1, lambda: None) is False
