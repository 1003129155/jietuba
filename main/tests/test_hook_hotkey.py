# -*- coding: utf-8 -*-
"""接管系统占用的组合键（Win+V）：绑定、派发、禁用时交还系统、按设置注册"""
from types import SimpleNamespace
from unittest.mock import Mock

import pytest

from core.input_hub import existing_input_hub, input_hub
from core.shortcut_manager import ShortcutManager
from main_app import MainApp
from settings.tool_settings import ToolSettingsManager

# 输入中心在测试里是不装钩子的原生状态机（见 conftest），按键由测试逐条给出。

VK_LWIN, VK_LSHIFT, VK_V = 0x5B, 0xA0, ord("V")


def _win_v() -> bool:
    """按下 Win+V 再全部松开，返回 V 的按下是否被吞掉。"""
    native = input_hub().native
    native.key(VK_LWIN, True)
    swallowed = native.key(VK_V, True)
    native.key(VK_V, False)
    native.key(VK_LWIN, False)
    return swallowed


def _hooks_needed() -> bool:
    hub = existing_input_hub()
    return hub is not None and hub.native.hooks_needed


class TestHookHotkey:
    def test_win_v_is_swallowed_and_dispatched_on_the_gui_thread(self, qapp):
        mgr = ShortcutManager()
        calls = []
        assert mgr.register_hook_hotkey("clipboard", ["win"], VK_V, lambda: calls.append("clipboard"))
        assert mgr.has_registered_hotkeys()

        assert _win_v() is True
        assert calls == []  # 经排队回到主线程
        qapp.processEvents()
        assert calls == ["clipboard"]

    def test_other_combinations_pass_through(self, qapp):
        mgr = ShortcutManager()
        mgr.register_hook_hotkey("clipboard", ["win"], VK_V, Mock())
        native = input_hub().native

        assert native.key(VK_V, True) is False
        native.key(VK_V, False)
        native.key(VK_LWIN, True)
        native.key(VK_LSHIFT, True)
        assert native.key(VK_V, True) is False

    def test_suppression_gives_win_v_back_to_the_system(self, qapp):
        mgr = ShortcutManager()
        callback = Mock()
        mgr.register_hook_hotkey("clipboard", ["win"], VK_V, callback)

        mgr.set_global_hotkeys_suppressed(True)
        assert not _hooks_needed()
        assert _win_v() is False
        assert mgr.has_registered_hotkeys(), "resuming must not need a re-register"

        mgr.set_global_hotkeys_suppressed(False)
        assert _win_v() is True
        qapp.processEvents()
        callback.assert_called_once()

    def test_an_event_queued_before_suppression_is_ignored(self, qapp):
        mgr = ShortcutManager()
        callback = Mock()
        mgr.register_hook_hotkey("clipboard", ["win"], VK_V, callback)
        _win_v()
        mgr.set_global_hotkeys_suppressed(True)
        qapp.processEvents()
        callback.assert_not_called()

    def test_registering_while_suppressed_waits_for_resume(self, qapp):
        mgr = ShortcutManager()
        mgr.set_global_hotkeys_suppressed(True)
        assert mgr.register_hook_hotkey("clipboard", ["win"], VK_V, Mock())
        assert existing_input_hub() is None, "nothing to bind yet"
        mgr.set_global_hotkeys_suppressed(False)
        assert _win_v() is True

    def test_unregister_all_gives_win_v_back(self, qapp):
        mgr = ShortcutManager()
        mgr.register_hook_hotkey("clipboard", ["win"], VK_V, Mock())
        mgr.unregister_all_hotkeys()
        assert not mgr.has_registered_hotkeys()
        assert not _hooks_needed()
        assert _win_v() is False

    def test_an_invalid_binding_is_reported(self, qapp):
        mgr = ShortcutManager()
        assert mgr.register_hook_hotkey("bare", [], VK_V, Mock()) is False


def _app(tmp_settings, *, take_over=None, clipboard_enabled=True):
    """take_over 为 None 时不写设置，走默认值。"""
    config = ToolSettingsManager(qsettings=tmp_settings)
    config.set_clipboard_enabled(clipboard_enabled)
    if take_over is not None:
        config.set_app_setting("clipboard_take_over_win_v", take_over)
    hotkeys = Mock()
    hotkeys.register_hotkey.return_value = True
    hotkeys.register_hook_hotkey.return_value = True
    return SimpleNamespace(
        config_manager=config, hotkey_system=hotkeys, tr=lambda text: text,
        start_screenshot=Mock(), open_clipboard_window=Mock(), pin_clipboard_image=Mock(),
        smart_translation_controller=SimpleNamespace(trigger=Mock()),
        quick_capture=Mock(), _show_hotkey_error=Mock(),
    )


class TestUpdateHotkey:
    @pytest.mark.parametrize("take_over", [True, None])
    def test_the_setting_takes_over_win_v_for_the_clipboard(self, tmp_settings, take_over):
        app = _app(tmp_settings, take_over=take_over)
        MainApp.update_hotkey(app)
        app.hotkey_system.register_hook_hotkey.assert_called_once_with(
            "clipboard", ["win"], VK_V, app.open_clipboard_window
        )

    @pytest.mark.parametrize("take_over, clipboard_enabled", [(False, True), (True, False)])
    def test_win_v_stays_with_the_system_otherwise(self, tmp_settings, take_over, clipboard_enabled):
        app = _app(tmp_settings, take_over=take_over, clipboard_enabled=clipboard_enabled)
        MainApp.update_hotkey(app)
        app.hotkey_system.register_hook_hotkey.assert_not_called()

    @pytest.mark.parametrize("change", ["clipboard_off", "take_over_off"])
    def test_turning_it_off_later_gives_win_v_back(self, tmp_settings, qapp, change):
        app = _app(tmp_settings)
        mgr = ShortcutManager()
        app.hotkey_system = SimpleNamespace(
            unregister_all=mgr.unregister_all_hotkeys,
            set_suppressed=mgr.set_global_hotkeys_suppressed,
            register_hotkey=Mock(return_value=True),
            register_hook_hotkey=mgr.register_hook_hotkey,
        )
        MainApp.update_hotkey(app)
        assert _win_v() is True

        if change == "clipboard_off":
            app.config_manager.set_clipboard_enabled(False)
        else:
            app.config_manager.set_app_setting("clipboard_take_over_win_v", False)
        MainApp.update_hotkey(app)

        assert _win_v() is False
        assert not _hooks_needed()

    def test_a_failed_take_over_is_shown_with_the_other_failures(self, tmp_settings, monkeypatch):
        monkeypatch.setattr("main_app.log_warning", Mock())
        app = _app(tmp_settings, take_over=True)
        app.hotkey_system.register_hook_hotkey.return_value = False
        MainApp.update_hotkey(app, show_error=True)
        app._show_hotkey_error.assert_called_once_with([("Clipboard", "win+v")])
