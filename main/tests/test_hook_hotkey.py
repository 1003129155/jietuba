# -*- coding: utf-8 -*-
"""钩子接管的全局热键（Win 组合键、PrtSc）：绑定、派发、系统注册兜底、禁用时交还系统、按设置注册"""
import ctypes
from types import SimpleNamespace
from unittest.mock import Mock

import pytest

from core.global_hotkey import MOD_NOREPEAT, MOD_WIN, VK_SNAPSHOT
from core.input_hub import existing_input_hub, input_hub
from core.shortcut_manager import ShortcutHandler, ShortcutManager
from main_app import MainApp
from settings.tool_settings import ToolSettingsManager

# 输入中心在测试里是不装钩子的原生状态机（见 conftest），按键由测试逐条给出。

VK_LWIN, VK_LSHIFT, VK_LCONTROL, VK_V = 0x5B, 0xA0, 0xA2, ord("V")


@pytest.fixture
def system_hotkeys(monkeypatch):
    """替掉 RegisterHotKey：测试不往系统里注册真热键。taken 里的组合当作被别人占着。"""
    state = SimpleNamespace(registered={}, taken=set())

    def register(_hwnd, hid, mods, vk):
        if (mods, vk) in state.taken or (mods, vk) in state.registered.values():
            return 0
        state.registered[hid] = (mods, vk)
        return 1

    def unregister(_hwnd, hid):
        return 1 if state.registered.pop(hid, None) else 0

    monkeypatch.setattr(ctypes.windll.user32, "RegisterHotKey", register)
    monkeypatch.setattr(ctypes.windll.user32, "UnregisterHotKey", unregister)
    yield state
    ShortcutManager._registered_keys_global.clear()


def _press(*vks, injected=False) -> bool:
    """依次按下 vks 再倒序松开，返回最后一个键的按下是否被吞掉。"""
    native = input_hub().native
    swallowed = False
    for vk in vks:
        swallowed = native.key(vk, True, injected=injected)
    for vk in reversed(vks):
        native.key(vk, False, injected=injected)
    return swallowed


def _win_v(injected=False) -> bool:
    return _press(VK_LWIN, VK_V, injected=injected)


def _hooks_needed() -> bool:
    hub = existing_input_hub()
    return hub is not None and hub.native.hooks_needed


class _Interceptor(ShortcutHandler):
    priority = 100

    def __init__(self):
        self.seen = []

    def is_active(self):
        return True

    def handle_key(self, event):
        return False

    def handle_hotkey(self, hotkey_id, callback):
        self.seen.append(callback)
        return True


class TestHookRoute:
    @pytest.mark.parametrize("injected", [False, True])
    def test_win_v_is_swallowed_and_dispatched_on_the_gui_thread(self, qapp, system_hotkeys, injected):
        system_hotkeys.taken.add((MOD_WIN | MOD_NOREPEAT, VK_V))
        mgr = ShortcutManager()
        calls = []
        assert mgr.register_hotkey("win+v", lambda: calls.append("clipboard"))
        assert mgr.has_registered_hotkeys()

        assert _win_v(injected) is True
        assert calls == []  # 经排队回到主线程
        qapp.processEvents()
        assert calls == ["clipboard"]

    def test_the_system_registration_backs_the_hook_up(self, qapp, system_hotkeys):
        mgr = ShortcutManager()
        assert mgr.register_hotkey("win+j", Mock())
        assert list(system_hotkeys.registered.values()) == [(MOD_WIN | MOD_NOREPEAT, ord("J"))]
        assert _press(VK_LWIN, ord("J")) is True

        mgr.unregister_all_hotkeys()
        assert system_hotkeys.registered == {}
        assert not _hooks_needed()

    def test_print_screen_binds_without_modifiers(self, qapp, system_hotkeys):
        mgr = ShortcutManager()
        callback = Mock()
        assert mgr.register_hotkey("print", callback)
        assert _press(VK_SNAPSHOT) is True
        assert _press(VK_LCONTROL, VK_SNAPSHOT) is False, "only the exact combination"
        qapp.processEvents()
        callback.assert_called_once()

    def test_handlers_can_intercept_like_a_system_hotkey(self, qapp, system_hotkeys):
        mgr = ShortcutManager()
        callback = Mock()
        interceptor = _Interceptor()
        mgr.register(interceptor)
        mgr.register_hotkey("win+t", callback)
        _press(VK_LWIN, ord("T"))
        qapp.processEvents()
        assert interceptor.seen == [callback]
        callback.assert_not_called()

    def test_other_combinations_pass_through(self, qapp, system_hotkeys):
        mgr = ShortcutManager()
        mgr.register_hotkey("win+v", Mock())
        native = input_hub().native

        assert native.key(VK_V, True) is False
        native.key(VK_V, False)
        native.key(VK_LWIN, True)
        native.key(VK_LSHIFT, True)
        assert native.key(VK_V, True) is False

    @pytest.mark.parametrize("injected", [False, True])
    def test_suppression_gives_win_v_back_to_the_system(self, qapp, system_hotkeys, injected):
        mgr = ShortcutManager()
        callback = Mock()
        mgr.register_hotkey("win+v", callback)

        mgr.set_global_hotkeys_suppressed(True)
        assert not _hooks_needed()
        assert _win_v(injected) is False
        assert mgr.has_registered_hotkeys(), "resuming must not need a re-register"

        mgr.set_global_hotkeys_suppressed(False)
        assert _win_v(injected) is True
        qapp.processEvents()
        callback.assert_called_once()

    def test_an_event_queued_before_suppression_is_ignored(self, qapp, system_hotkeys):
        mgr = ShortcutManager()
        callback = Mock()
        mgr.register_hotkey("win+v", callback)
        _win_v()
        mgr.set_global_hotkeys_suppressed(True)
        qapp.processEvents()
        callback.assert_not_called()

    def test_registering_while_suppressed_waits_for_resume(self, qapp, system_hotkeys):
        system_hotkeys.taken.add((MOD_WIN | MOD_NOREPEAT, VK_V))
        mgr = ShortcutManager()
        mgr.set_global_hotkeys_suppressed(True)
        assert mgr.register_hotkey("win+v", Mock())
        assert existing_input_hub() is None, "nothing to bind yet"
        mgr.set_global_hotkeys_suppressed(False)
        assert _win_v() is True

    @pytest.mark.parametrize("injected", [False, True])
    def test_unregister_all_gives_win_v_back(self, qapp, system_hotkeys, injected):
        mgr = ShortcutManager()
        mgr.register_hotkey("win+v", Mock())
        mgr.unregister_all_hotkeys()
        assert not mgr.has_registered_hotkeys()
        assert not _hooks_needed()
        assert _win_v(injected) is False

    def test_simulated_ctrl_v_paste_does_not_trigger_win_v(self, qapp, system_hotkeys):
        mgr = ShortcutManager()
        callback = Mock()
        mgr.register_hotkey("win+v", callback)
        native = input_hub().native
        for vk, pressed in [(VK_LCONTROL, True), (VK_V, True), (VK_V, False), (VK_LCONTROL, False)]:
            assert native.key(vk, pressed, injected=True) is False
        qapp.processEvents()
        callback.assert_not_called()


class TestOtherRoutes:
    def test_ordinary_combinations_only_use_the_system_registration(self, qapp, system_hotkeys):
        mgr = ShortcutManager()
        assert mgr.register_hotkey("ctrl+alt+a", Mock())
        assert existing_input_hub() is None, "no hook for keys the system registration covers"

    def test_a_combination_held_by_another_app_fails(self, qapp, system_hotkeys):
        system_hotkeys.taken.add((0x2 | 0x1 | MOD_NOREPEAT, ord("A")))
        mgr = ShortcutManager()
        assert mgr.register_hotkey("ctrl+alt+a", Mock()) is False
        assert not mgr.has_registered_hotkeys()

    @pytest.mark.parametrize("hotkey", ["win+l", "ctrl+alt+delete", "a", "ctrl+!", "ctrl+"])
    def test_reserved_and_invalid_values_register_nothing(self, qapp, system_hotkeys, hotkey):
        mgr = ShortcutManager()
        assert mgr.register_hotkey(hotkey, Mock()) is False
        assert system_hotkeys.registered == {}
        assert not mgr.has_registered_hotkeys()


class TestKeyCapture:
    class _Recorder(ShortcutHandler):
        priority = 200

        def __init__(self):
            self.captured = []

        def is_active(self):
            return True

        def handle_key(self, event):
            return False

        def handle_captured_key(self, text):
            self.captured.append(text)
            return True

    def test_recording_takes_win_combinations_and_print_screen(self, qapp):
        mgr = ShortcutManager()
        recorder = self._Recorder()
        mgr.register(recorder)
        mgr.begin_key_capture()
        assert _press(VK_LWIN, VK_LSHIFT, ord("S")) is True
        assert _press(VK_SNAPSHOT) is True
        assert _press(VK_LCONTROL, ord("S")) is False, "the edit records this one itself"
        qapp.processEvents()
        assert recorder.captured == ["shift+win+s", "print"]

        mgr.end_key_capture()
        assert _press(VK_LWIN, ord("S")) is False
        assert not _hooks_needed()

    def test_capture_is_reference_counted(self, qapp):
        mgr = ShortcutManager()
        mgr.begin_key_capture()
        mgr.begin_key_capture()
        mgr.end_key_capture()
        assert _press(VK_LWIN, VK_V) is True
        mgr.end_key_capture()
        mgr.end_key_capture()  # 多放一次不出错
        assert _press(VK_LWIN, VK_V) is False


def _app(tmp_settings, *, take_over=None, clipboard_enabled=True):
    """take_over 为 None 时不写设置，走默认值。"""
    config = ToolSettingsManager(qsettings=tmp_settings)
    config.set_clipboard_enabled(clipboard_enabled)
    if take_over is not None:
        config.set_app_setting("clipboard_take_over_win_v", take_over)
    hotkeys = Mock()
    hotkeys.register_hotkey.return_value = True
    return SimpleNamespace(
        config_manager=config, hotkey_system=hotkeys, tr=lambda text: text,
        start_screenshot=Mock(), open_clipboard_window=Mock(), pin_clipboard_image=Mock(),
        smart_translation_controller=SimpleNamespace(trigger=Mock()),
        quick_capture=Mock(), _show_hotkey_error=Mock(),
    )


def _registered(app):
    return [call.args for call in app.hotkey_system.register_hotkey.call_args_list]


class TestUpdateHotkey:
    @pytest.mark.parametrize("take_over", [True, None])
    def test_the_old_win_v_setting_fills_the_third_clipboard_slot(self, tmp_settings, take_over):
        app = _app(tmp_settings, take_over=take_over)
        MainApp.update_hotkey(app)
        assert ("win+v", app.open_clipboard_window) in _registered(app)

    @pytest.mark.parametrize("take_over, clipboard_enabled", [(False, True), (True, False)])
    def test_win_v_stays_with_the_system_otherwise(self, tmp_settings, take_over, clipboard_enabled):
        app = _app(tmp_settings, take_over=take_over, clipboard_enabled=clipboard_enabled)
        MainApp.update_hotkey(app)
        assert all(hotkey != "win+v" for hotkey, _callback in _registered(app))

    def test_every_slot_of_every_function_is_registered(self, tmp_settings):
        app = _app(tmp_settings)
        config = app.config_manager
        config.set_global_hotkeys("screenshot", ["ctrl+1", "", "print", "win+shift+s"])
        config.set_global_hotkeys("translation", ["", "", "", "ctrl+alt+t"])
        config.set_global_hotkeys("clipboard", ["ctrl+2", "", "", ""])
        config.set_global_hotkeys("pin_clipboard", ["", "mouseback", "", ""])
        MainApp.update_hotkey(app)
        assert _registered(app) == [
            ("ctrl+1", app.start_screenshot),
            ("print", app.start_screenshot),
            ("win+shift+s", app.start_screenshot),
            ("ctrl+alt+t", app.smart_translation_controller.trigger),
            ("ctrl+2", app.open_clipboard_window),
            ("mouseback", app.pin_clipboard_image),
        ]

    @pytest.mark.parametrize("change", ["clipboard_off", "slot_cleared"])
    def test_turning_it_off_later_gives_win_v_back(self, tmp_settings, qapp, system_hotkeys, change):
        app = _app(tmp_settings)
        mgr = ShortcutManager()
        app.hotkey_system = SimpleNamespace(
            unregister_all=mgr.unregister_all_hotkeys,
            set_suppressed=mgr.set_global_hotkeys_suppressed,
            register_hotkey=mgr.register_hotkey,
        )
        MainApp.update_hotkey(app)
        assert _win_v() is True

        if change == "clipboard_off":
            app.config_manager.set_clipboard_enabled(False)
        else:
            app.config_manager.set_global_hotkeys("clipboard", ["ctrl+2", "", "", ""])
        MainApp.update_hotkey(app)

        assert _win_v() is False
        assert not _hooks_needed()

    def test_failures_name_the_slot(self, tmp_settings, monkeypatch):
        monkeypatch.setattr("main_app.log_warning", Mock())
        app = _app(tmp_settings, take_over=True)
        app.hotkey_system.register_hotkey.side_effect = lambda hotkey, _cb: hotkey != "win+v"
        MainApp.update_hotkey(app, show_error=True)
        app._show_hotkey_error.assert_called_once_with([("Clipboard (3)", "win+v")])
