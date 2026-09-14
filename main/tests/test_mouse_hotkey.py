# -*- coding: utf-8 -*-
"""鼠标侧键快捷键：登记表、低级钩子的启停与独占、派发链、录入框捕获"""
import pytest

from pynput import mouse as _pynput_mouse

from core.shortcut_manager import (
    ShortcutManager, ShortcutHandler, HotkeySystem,
    MOUSE_BUTTON_BACK, MOUSE_BUTTON_FORWARD,
    is_mouse_button_hotkey,
)

# 在任何 monkeypatch 之前抓住真实的 Listener：下面的替身会把
# pynput.mouse.Listener 整个顶替掉，届时再去读这些常量读到的就是替身自己。
_REAL_LISTENER = _pynput_mouse.Listener
WM_XBUTTONDOWN = _REAL_LISTENER.WM_XBUTTONDOWN
WM_XBUTTONUP = _REAL_LISTENER.WM_XBUTTONUP
WM_MOUSEMOVE = _REAL_LISTENER.WM_MOUSEMOVE
WM_LBUTTONDOWN = _REAL_LISTENER.WM_LBUTTONDOWN


# ============================================================================
# 替身
# ============================================================================
# 真实的 pynput.mouse.Listener 会挂一个系统级 WH_MOUSE_LL 钩子，测试里既不该挂
# 也没法断言。替身复刻它被我们用到的那部分接口：X_BUTTONS 映射表（直接借用
# pynput 的真表，免得测试里再抄一份 Windows 常量）、start/stop、以及会抛异常的
# suppress_event。

class _Suppressed(Exception):
    """替身版 SystemHook.SuppressException。"""


class _FakeMouseListener:
    instances = []

    def __init__(self, win32_event_filter=None, **_kwargs):
        self.X_BUTTONS = _REAL_LISTENER.X_BUTTONS
        self.win32_event_filter = win32_event_filter
        self.started = False
        self.stopped = False
        _FakeMouseListener.instances.append(self)

    def start(self):
        self.started = True

    def stop(self):
        self.stopped = True

    def suppress_event(self):
        raise _Suppressed()


class _FakeHookData:
    """MSLLHOOKSTRUCT 里我们唯一读到的字段。"""

    def __init__(self, xbutton_index):
        self.mouseData = xbutton_index << 16


def _hook_event(mgr, msg, xbutton_index) -> bool:
    """模拟一次钩子回调，返回该事件是否被吞掉（True = 其它程序收不到）。"""
    try:
        mgr._mouse_event_filter(msg, _FakeHookData(xbutton_index))
    except _Suppressed:
        return True
    return False


@pytest.fixture
def clean_mouse_registry():
    """鼠标侧键登记表是类级变量，测试间需要手动清理，避免互相污染。"""
    yield
    ShortcutManager._registered_mouse_buttons_global.clear()


@pytest.fixture
def fake_listener(monkeypatch):
    """用替身顶替 pynput.mouse.Listener，并返回已创建实例的列表。"""
    _FakeMouseListener.instances = []
    monkeypatch.setattr("pynput.mouse.Listener", _FakeMouseListener)
    return _FakeMouseListener.instances


@pytest.fixture
def no_real_mouse_listener(monkeypatch):
    """只关心登记/派发语义、不关心钩子的用例，直接屏蔽掉启动。"""
    monkeypatch.setattr(ShortcutManager, "_start_mouse_listener", lambda self: None)


# ============================================================================
# is_mouse_button_hotkey
# ============================================================================

class TestIsMouseButtonHotkey:
    def test_recognizes_known_tokens_case_insensitively(self):
        assert is_mouse_button_hotkey("mouseback")
        assert is_mouse_button_hotkey("MouseForward")
        assert is_mouse_button_hotkey("  mouseback  ")

    def test_rejects_keyboard_strings(self):
        assert not is_mouse_button_hotkey("ctrl+shift+a")
        assert not is_mouse_button_hotkey("f1")
        assert not is_mouse_button_hotkey("")
        assert not is_mouse_button_hotkey(None)


# ============================================================================
# 登记语义
# ============================================================================

class TestMouseHotkeyRegistration:
    def test_register_then_duplicate_is_rejected(self, clean_mouse_registry, no_real_mouse_listener):
        mgr = ShortcutManager()
        assert mgr.register_hotkey(MOUSE_BUTTON_BACK, lambda: None) is True
        assert mgr.has_registered_hotkeys() is True
        # 同一 token 被同进程重复注册，语义对齐 RegisterHotKey 拒绝重复注册
        assert mgr.register_hotkey(MOUSE_BUTTON_BACK, lambda: None) is False

    def test_different_tokens_both_register(self, clean_mouse_registry, no_real_mouse_listener):
        mgr = ShortcutManager()
        assert mgr.register_hotkey(MOUSE_BUTTON_BACK, lambda: None) is True
        assert mgr.register_hotkey(MOUSE_BUTTON_FORWARD, lambda: None) is True

    def test_unregister_all_clears_mouse_registry(self, clean_mouse_registry, no_real_mouse_listener):
        mgr = ShortcutManager()
        mgr.register_hotkey(MOUSE_BUTTON_BACK, lambda: None)
        mgr.unregister_all_hotkeys()
        assert mgr.has_registered_hotkeys() is False
        assert MOUSE_BUTTON_BACK not in ShortcutManager._registered_mouse_buttons_global

    def test_availability_always_true_regardless_of_registration(
        self, clean_mouse_registry, no_real_mouse_listener
    ):
        hs = HotkeySystem()
        try:
            assert hs.check_hotkey_availability(MOUSE_BUTTON_BACK) is True
            hs.register_hotkey(MOUSE_BUTTON_BACK, lambda: None)
            # 鼠标侧键没有系统级冲突探测手段，即使已被本进程占用也不应显示为冲突
            assert hs.check_hotkey_availability(MOUSE_BUTTON_BACK) is True
        finally:
            hs.unregister_all()


# ============================================================================
# 钩子生命周期：什么时候挂、什么时候必须摘
# ============================================================================
# 钩子挂着就意味着侧键被我们独占，其它程序（浏览器的后退/前进）收不到。所以
# 「该摘的时候摘掉」和「该挂的时候挂上」同等重要——漏摘的表现是用户解绑之后
# 浏览器后退键永远失灵，而且没有任何报错。

class TestMouseHookLifecycle:
    def test_hook_starts_on_first_binding_and_is_shared(self, clean_mouse_registry, fake_listener):
        mgr = ShortcutManager()
        mgr.register_hotkey(MOUSE_BUTTON_BACK, lambda: None)
        assert len(fake_listener) == 1
        assert fake_listener[0].started is True

        # 第二个 token 复用同一条监听线程，不该再起一个
        mgr.register_hotkey(MOUSE_BUTTON_FORWARD, lambda: None)
        assert len(fake_listener) == 1

    def test_hook_is_removed_when_last_binding_goes_away(self, clean_mouse_registry, fake_listener):
        """解绑后必须摘钩子，否则侧键再也回不到其它程序手里。"""
        mgr = ShortcutManager()
        mgr.register_hotkey(MOUSE_BUTTON_BACK, lambda: None)
        mgr.unregister_all_hotkeys()

        assert fake_listener[0].stopped is True
        assert mgr._mouse_listener is None
        assert mgr._suppressed_mouse_tokens == frozenset()

    def test_no_hook_when_nothing_is_bound(self, clean_mouse_registry, fake_listener):
        mgr = ShortcutManager()
        mgr.register_hotkey("ctrl+shift+f9", lambda: None)  # 键盘热键不该牵动鼠标钩子
        assert fake_listener == []

    def test_capture_holds_the_hook_open_with_no_bindings(self, clean_mouse_registry, fake_listener):
        """录入框聚焦时即使一个侧键都没绑，也要挂钩子，否则根本录不到。"""
        mgr = ShortcutManager()
        mgr.begin_mouse_capture()
        assert len(fake_listener) == 1
        assert mgr._suppressed_mouse_tokens == frozenset(
            {MOUSE_BUTTON_BACK, MOUSE_BUTTON_FORWARD}
        )

        mgr.end_mouse_capture()
        assert fake_listener[0].stopped is True
        assert mgr._suppressed_mouse_tokens == frozenset()

    def test_capture_is_reference_counted(self, clean_mouse_registry, fake_listener):
        """两个录入框先后聚焦时，先失焦的那个不能把钩子提前摘掉。"""
        mgr = ShortcutManager()
        mgr.begin_mouse_capture()
        mgr.begin_mouse_capture()
        mgr.end_mouse_capture()
        assert fake_listener[0].stopped is False

        mgr.end_mouse_capture()
        assert fake_listener[0].stopped is True

    def test_end_capture_is_ignored_when_not_held(self, clean_mouse_registry, fake_listener):
        """多余的释放不该把计数压成负数，否则后续 begin 会失效。"""
        mgr = ShortcutManager()
        mgr.end_mouse_capture()
        assert mgr._mouse_capture_refs == 0
        mgr.begin_mouse_capture()
        assert len(fake_listener) == 1

    def test_unregister_during_capture_keeps_the_hook(self, clean_mouse_registry, fake_listener):
        """在设置窗口里改热键：解绑会走 unregister_all，但录入框还聚焦着。"""
        mgr = ShortcutManager()
        mgr.register_hotkey(MOUSE_BUTTON_BACK, lambda: None)
        mgr.begin_mouse_capture()
        mgr.unregister_all_hotkeys()

        assert fake_listener[0].stopped is False
        # 录入期间仍独占全部侧键
        assert mgr._suppressed_mouse_tokens == frozenset(
            {MOUSE_BUTTON_BACK, MOUSE_BUTTON_FORWARD}
        )

        mgr.end_mouse_capture()
        assert fake_listener[0].stopped is True

    def test_binding_survives_capture_release(self, clean_mouse_registry, fake_listener):
        """录入结束后已绑定的 token 仍要独占，未绑定的要放行。"""
        mgr = ShortcutManager()
        mgr.register_hotkey(MOUSE_BUTTON_BACK, lambda: None)
        mgr.begin_mouse_capture()
        mgr.end_mouse_capture()

        assert fake_listener[0].stopped is False
        assert mgr._suppressed_mouse_tokens == frozenset({MOUSE_BUTTON_BACK})


# ============================================================================
# 钩子回调：映射、独占范围、成对抑制
# ============================================================================

class TestMouseEventFilter:
    def test_bound_button_is_taken_from_the_rest_of_the_system(
        self, clean_mouse_registry, fake_listener
    ):
        mgr = ShortcutManager()
        mgr.register_hotkey(MOUSE_BUTTON_BACK, lambda: None)
        down, up = WM_XBUTTONDOWN, WM_XBUTTONUP

        # 按下与抬起都要吞掉：只吞 DOWN 会让其它程序收到没有配对按下的抬起
        assert _hook_event(mgr, down, 1) is True
        assert _hook_event(mgr, up, 1) is True

    def test_unbound_button_passes_through(self, clean_mouse_registry, fake_listener):
        """只绑了后退键时，前进键必须照常交给浏览器。"""
        mgr = ShortcutManager()
        mgr.register_hotkey(MOUSE_BUTTON_BACK, lambda: None)
        down = WM_XBUTTONDOWN

        assert _hook_event(mgr, down, 2) is False

    def test_capture_takes_every_side_button(self, clean_mouse_registry, fake_listener):
        """录入期间两个侧键都独占，免得录这一下顺带让后台浏览器退一页。"""
        mgr = ShortcutManager()
        mgr.begin_mouse_capture()
        down = WM_XBUTTONDOWN

        assert _hook_event(mgr, down, 1) is True
        assert _hook_event(mgr, down, 2) is True

    def test_press_emits_token_and_release_does_not(self, clean_mouse_registry, fake_listener):
        mgr = ShortcutManager()
        received = []
        mgr._mouse_button_triggered.connect(received.append)
        mgr.begin_mouse_capture()
        down, up = WM_XBUTTONDOWN, WM_XBUTTONUP

        _hook_event(mgr, down, 1)
        _hook_event(mgr, up, 1)
        _hook_event(mgr, down, 2)

        assert received == [MOUSE_BUTTON_BACK, MOUSE_BUTTON_FORWARD]

    def test_other_mouse_messages_are_left_alone(self, clean_mouse_registry, fake_listener):
        """移动和左键必须原样放行，不能因为钩子挂着就影响正常操作。"""
        mgr = ShortcutManager()
        mgr.begin_mouse_capture()

        assert _hook_event(mgr, WM_MOUSEMOVE, 0) is False
        assert _hook_event(mgr, WM_LBUTTONDOWN, 0) is False

    def test_filter_passes_through_after_the_hook_is_torn_down(
        self, clean_mouse_registry, fake_listener
    ):
        """停止过程中还在路上的事件不能被吞掉。"""
        mgr = ShortcutManager()
        mgr.register_hotkey(MOUSE_BUTTON_BACK, lambda: None)
        mgr.unregister_all_hotkeys()

        assert _hook_event(mgr, WM_XBUTTONDOWN, 1) is False


# ============================================================================
# 主线程派发
# ============================================================================

class TestMouseDispatch:
    def test_triggered_slot_invokes_callback(self, clean_mouse_registry, no_real_mouse_listener):
        mgr = ShortcutManager()
        calls = []
        mgr.register_hotkey(MOUSE_BUTTON_BACK, lambda: calls.append("fired"))
        mgr._on_mouse_button_triggered(MOUSE_BUTTON_BACK)
        assert calls == ["fired"]

    def test_triggered_slot_respects_global_suppression(
        self, clean_mouse_registry, no_real_mouse_listener
    ):
        mgr = ShortcutManager()
        calls = []
        mgr.register_hotkey(MOUSE_BUTTON_BACK, lambda: calls.append("fired"))
        mgr.set_global_hotkeys_suppressed(True)
        mgr._on_mouse_button_triggered(MOUSE_BUTTON_BACK)
        assert calls == []

    def test_handler_chain_runs_for_unbound_buttons(self, clean_mouse_registry, no_real_mouse_listener):
        """录入的前提：没绑任何功能的侧键也要经过 handler 链。"""
        mgr = ShortcutManager()
        seen = []

        class _Recorder(ShortcutHandler):
            def is_active(self):
                return True

            def handle_key(self, event):
                return False

            def handle_mouse_hotkey(self, token, callback):
                seen.append((token, callback))
                return True

            @property
            def priority(self):
                return 999

        mgr.register(_Recorder())
        mgr._on_mouse_button_triggered(MOUSE_BUTTON_FORWARD)

        assert seen == [(MOUSE_BUTTON_FORWARD, None)]

    def test_handler_chain_can_intercept_bound_buttons(
        self, clean_mouse_registry, no_real_mouse_listener
    ):
        mgr = ShortcutManager()
        calls = []
        mgr.register_hotkey(MOUSE_BUTTON_BACK, lambda: calls.append("fired"))

        class _Interceptor(ShortcutHandler):
            def is_active(self):
                return True

            def handle_key(self, event):
                return False

            def handle_mouse_hotkey(self, token, callback):
                return True

            @property
            def priority(self):
                return 999

        mgr.register(_Interceptor())
        mgr._on_mouse_button_triggered(MOUSE_BUTTON_BACK)
        assert calls == []


# ============================================================================
# 录入框
# ============================================================================
# 侧键在录入期间被钩子整体吞掉，Qt 收不到 mousePressEvent，所以录入的唯一入口
# 是 handler 链上的 handle_mouse_hotkey。

class TestHotkeyEditMouseCapture:
    def test_side_button_overwrites_pending_keyboard_prefix(self, qapp):
        from ui.hotkey_edit import HotkeyEdit
        widget = HotkeyEdit()
        widget.setText("ctrl+shift+")  # 模拟正在输入一半的键盘组合
        widget.edit._shortcut_handler.handle_mouse_hotkey(MOUSE_BUTTON_BACK, None)
        assert widget.text() == MOUSE_BUTTON_BACK

    def test_side_button_is_always_intercepted(self, qapp):
        """录入期间这一下不能同时触发被绑定功能的原回调。"""
        from ui.hotkey_edit import HotkeyEdit
        widget = HotkeyEdit()
        handled = widget.edit._shortcut_handler.handle_mouse_hotkey(
            MOUSE_BUTTON_FORWARD, lambda: None
        )
        assert handled is True
        assert widget.text() == MOUSE_BUTTON_FORWARD

    def test_mouse_token_reports_available_not_conflicting(self, qapp):
        # 录入框用 ShortcutManager._parse_hotkey 检测冲突，鼠标 token 走不通那条
        # 键盘专属的解析路径——回归点在于它不该被误判为「已被占用」。
        from ui.hotkey_edit import HotkeyEdit
        widget = HotkeyEdit()
        widget.edit._shortcut_handler.handle_mouse_hotkey(MOUSE_BUTTON_BACK, None)
        assert widget.text() == MOUSE_BUTTON_BACK
        widget._check_availability_now()
        assert widget.status_lbl.text() == "✅"

    def test_focus_acquires_and_releases_the_side_button_hold(self, qapp, clean_mouse_registry):
        """失焦必须把独占还回去，否则用户关掉设置窗口后侧键还在被我们吞。"""
        from PySide6.QtCore import QEvent
        from PySide6.QtGui import QFocusEvent
        from ui.hotkey_edit import HotkeyEdit

        mgr = ShortcutManager.instance()
        before = mgr._mouse_capture_refs
        widget = HotkeyEdit()

        widget.edit.focusInEvent(QFocusEvent(QEvent.Type.FocusIn))
        assert mgr._mouse_capture_refs == before + 1

        widget.edit.focusOutEvent(QFocusEvent(QEvent.Type.FocusOut))
        assert mgr._mouse_capture_refs == before
