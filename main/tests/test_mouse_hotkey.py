# -*- coding: utf-8 -*-
"""鼠标侧键快捷键：登记表、低级钩子的启停与独占、派发链、录入框捕获"""
import pytest

from core.input_hub import existing_input_hub, input_hub
from core.shortcut_manager import (
    ShortcutManager, ShortcutHandler, HotkeySystem,
    MOUSE_BUTTON_BACK, MOUSE_BUTTON_FORWARD,
    is_mouse_button_hotkey,
)

# 输入中心在测试里是不装钩子的原生状态机（见 conftest），侧键输入由测试逐条给出。


def _side(button, pressed=True, injected=False) -> bool:
    """模拟一次侧键输入，返回它是否被吞掉（True = 其它程序收不到）。"""
    return input_hub().native.mouse("down" if pressed else "up", button=button, injected=injected)


def _hooks_needed() -> bool:
    hub = existing_input_hub()
    return hub is not None and hub.native.hooks_needed


@pytest.fixture
def clean_mouse_registry():
    """鼠标侧键登记表是类级变量，测试间需要手动清理，避免互相污染。"""
    yield
    ShortcutManager._registered_mouse_buttons_global.clear()


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
    def test_register_then_duplicate_is_rejected(self, clean_mouse_registry):
        mgr = ShortcutManager()
        assert mgr.register_hotkey(MOUSE_BUTTON_BACK, lambda: None) is True
        assert mgr.has_registered_hotkeys() is True
        # 同一 token 被同进程重复注册，语义对齐 RegisterHotKey 拒绝重复注册
        assert mgr.register_hotkey(MOUSE_BUTTON_BACK, lambda: None) is False

    def test_different_tokens_both_register(self, clean_mouse_registry):
        mgr = ShortcutManager()
        assert mgr.register_hotkey(MOUSE_BUTTON_BACK, lambda: None) is True
        assert mgr.register_hotkey(MOUSE_BUTTON_FORWARD, lambda: None) is True

    def test_unregister_all_clears_mouse_registry(self, clean_mouse_registry):
        mgr = ShortcutManager()
        mgr.register_hotkey(MOUSE_BUTTON_BACK, lambda: None)
        mgr.unregister_all_hotkeys()
        assert mgr.has_registered_hotkeys() is False
        assert MOUSE_BUTTON_BACK not in ShortcutManager._registered_mouse_buttons_global

    def test_availability_always_true_regardless_of_registration(
        self, clean_mouse_registry
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
    def test_hook_starts_on_first_binding(self, clean_mouse_registry):
        mgr = ShortcutManager()
        mgr.register_hotkey(MOUSE_BUTTON_BACK, lambda: None)
        assert _hooks_needed()
        mgr.register_hotkey(MOUSE_BUTTON_FORWARD, lambda: None)
        assert _hooks_needed()

    def test_hook_is_removed_when_last_binding_goes_away(self, clean_mouse_registry):
        """解绑后必须摘钩子，否则侧键再也回不到其它程序手里。"""
        mgr = ShortcutManager()
        mgr.register_hotkey(MOUSE_BUTTON_BACK, lambda: None)
        mgr.unregister_all_hotkeys()

        assert not _hooks_needed()
        assert _side("x1") is False

    def test_no_input_hub_when_nothing_is_bound(self, clean_mouse_registry):
        mgr = ShortcutManager()
        mgr.register_hotkey("ctrl+shift+f9", lambda: None)  # 键盘热键不该牵动鼠标钩子
        mgr.unregister_all_hotkeys()
        assert existing_input_hub() is None

    def test_capture_holds_the_hook_open_with_no_bindings(self, clean_mouse_registry):
        """录入框聚焦时即使一个侧键都没绑，也要挂钩子，否则根本录不到。"""
        mgr = ShortcutManager()
        mgr.begin_mouse_capture()
        assert _hooks_needed()
        assert _side("x1") is True and _side("x1", pressed=False) is True
        assert _side("x2") is True and _side("x2", pressed=False) is True

        mgr.end_mouse_capture()
        assert not _hooks_needed()

    def test_capture_is_reference_counted(self, clean_mouse_registry):
        """两个录入框先后聚焦时，先失焦的那个不能把钩子提前摘掉。"""
        mgr = ShortcutManager()
        mgr.begin_mouse_capture()
        mgr.begin_mouse_capture()
        mgr.end_mouse_capture()
        assert _hooks_needed()

        mgr.end_mouse_capture()
        assert not _hooks_needed()

    def test_end_capture_is_ignored_when_not_held(self, clean_mouse_registry):
        """多余的释放不该把计数压成负数，否则后续 begin 会失效。"""
        mgr = ShortcutManager()
        mgr.end_mouse_capture()
        assert mgr._mouse_capture_refs == 0
        mgr.begin_mouse_capture()
        assert _hooks_needed()

    def test_unregister_during_capture_keeps_the_hook(self, clean_mouse_registry):
        """在设置窗口里改热键：解绑会走 unregister_all，但录入框还聚焦着。"""
        mgr = ShortcutManager()
        mgr.register_hotkey(MOUSE_BUTTON_BACK, lambda: None)
        mgr.begin_mouse_capture()
        mgr.unregister_all_hotkeys()

        # 录入期间仍独占全部侧键
        assert _side("x2") is True and _side("x2", pressed=False) is True

        mgr.end_mouse_capture()
        assert not _hooks_needed()

    def test_binding_survives_capture_release(self, clean_mouse_registry):
        """录入结束后已绑定的 token 仍要独占，未绑定的要放行。"""
        mgr = ShortcutManager()
        mgr.register_hotkey(MOUSE_BUTTON_BACK, lambda: None)
        mgr.begin_mouse_capture()
        mgr.end_mouse_capture()

        assert _side("x1") is True and _side("x1", pressed=False) is True
        assert _side("x2") is False


# ============================================================================
# 钩子上的判断：独占范围、成对抑制、上报
# ============================================================================

class TestSideButtonInput:
    def test_bound_button_is_taken_from_the_rest_of_the_system(self, clean_mouse_registry):
        mgr = ShortcutManager()
        mgr.register_hotkey(MOUSE_BUTTON_BACK, lambda: None)

        # 按下与抬起都要吞掉：只吞 DOWN 会让其它程序收到没有配对按下的抬起
        assert _side("x1") is True
        assert _side("x1", pressed=False) is True

    def test_unbound_button_passes_through(self, clean_mouse_registry):
        """只绑了后退键时，前进键必须照常交给浏览器。"""
        mgr = ShortcutManager()
        mgr.register_hotkey(MOUSE_BUTTON_BACK, lambda: None)

        assert _side("x2") is False
        assert _side("x2", pressed=False) is False

    def test_release_after_unbinding_is_still_swallowed(self, clean_mouse_registry):
        """按下已被吞掉时解绑，抬起也要吞，别的程序不能收到没有按下的抬起。"""
        mgr = ShortcutManager()
        mgr.register_hotkey(MOUSE_BUTTON_BACK, lambda: None)
        assert _side("x1") is True
        mgr.unregister_all_hotkeys()
        assert _side("x1", pressed=False) is True
        assert not _hooks_needed()
        assert _side("x1") is False

    def test_press_is_dispatched_once_and_release_is_not(self, qapp, clean_mouse_registry):
        mgr = ShortcutManager()
        calls = []
        mgr.register_hotkey(MOUSE_BUTTON_BACK, lambda: calls.append("back"))
        mgr.register_hotkey(MOUSE_BUTTON_FORWARD, lambda: calls.append("forward"))

        _side("x1")
        _side("x1", pressed=False)
        _side("x2")
        assert calls == []  # 经排队回到主线程
        qapp.processEvents()
        assert calls == ["back", "forward"]

    def test_injected_side_buttons_count(self, qapp, clean_mouse_registry):
        """鼠标驱动软件常用模拟输入发侧键，照样要生效。"""
        mgr = ShortcutManager()
        calls = []
        mgr.register_hotkey(MOUSE_BUTTON_BACK, lambda: calls.append("back"))

        assert _side("x1", injected=True) is True
        assert _side("x1", pressed=False, injected=True) is True
        qapp.processEvents()
        assert calls == ["back"]

    def test_other_mouse_input_is_left_alone(self, clean_mouse_registry):
        """移动和左键必须原样放行，不能因为钩子挂着就影响正常操作。"""
        mgr = ShortcutManager()
        mgr.begin_mouse_capture()
        native = input_hub().native

        assert native.mouse("move") is False
        assert native.mouse("down", button="left") is False
        assert native.mouse("up", button="left") is False


# ============================================================================
# 主线程派发
# ============================================================================

class TestMouseDispatch:
    def test_triggered_slot_invokes_callback(self, clean_mouse_registry):
        mgr = ShortcutManager()
        calls = []
        mgr.register_hotkey(MOUSE_BUTTON_BACK, lambda: calls.append("fired"))
        mgr._on_mouse_button_triggered(MOUSE_BUTTON_BACK)
        assert calls == ["fired"]

    def test_triggered_slot_respects_global_suppression(
        self, clean_mouse_registry
    ):
        mgr = ShortcutManager()
        calls = []
        mgr.register_hotkey(MOUSE_BUTTON_BACK, lambda: calls.append("fired"))
        mgr.set_global_hotkeys_suppressed(True)
        mgr._on_mouse_button_triggered(MOUSE_BUTTON_BACK)
        assert calls == []

    def test_handler_chain_runs_for_unbound_buttons(self, clean_mouse_registry):
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
        self, clean_mouse_registry
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
        assert widget.status_state == "ok"

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
