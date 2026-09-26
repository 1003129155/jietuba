"""MainApp keeps the quick-capture listener in sync with app lifecycle changes."""

from types import MethodType, SimpleNamespace
from unittest.mock import Mock

import pytest

from main_app import MainApp


@pytest.fixture
def app(monkeypatch):
    for name in ("log_info", "log_debug", "log_exception"):
        monkeypatch.setattr(f"main_app.{name}", Mock())
    saved = {"global_hotkeys_disabled": False, "quick_capture_action": "copy_pin"}
    config = SimpleNamespace(
        get_app_setting=lambda key, default=None: saved.get(key, default),
        set_app_setting=lambda key, value: saved.__setitem__(key, value),
        get_hotkey=lambda: "",
        get_hotkey_2=lambda: "",
        get_translation_hotkey=lambda: "",
        get_translation_hotkey_2=lambda: "",
        get_pin_clipboard_hotkey=lambda: "",
        get_pin_clipboard_hotkey_2=lambda: "",
        get_clipboard_enabled=lambda: False,
    )
    instance = SimpleNamespace(
        config_manager=config,
        saved_settings=saved,
        hotkey_system=Mock(),
        quick_capture=Mock(spec=["busy", "refresh", "suspend", "close", "set_capture_pending",
                                "capture_preparation_finished"]),
        screenshot_window=None,
        clipboard_window=None,
        clipboard_manager=None,
        settings_window=None,
        _capture_thread=None,
        _logger=Mock(),
        app=Mock(),
        tr=lambda text: text,
    )
    instance.quick_capture.busy = False
    instance.hotkey_system.has_registered_hotkeys.return_value = True
    instance.update_hotkey = MethodType(MainApp.update_hotkey, instance)
    return instance


def test_applying_hotkey_settings_refreshes_quick_capture_with_current_preferences(app):
    applied = []
    app.quick_capture.refresh.side_effect = lambda: applied.append(dict(app.saved_settings))
    app.update_hotkey()
    app.saved_settings["quick_capture_action"] = "pin"
    app.saved_settings["quick_capture_modifier_1"] = "ctrl"
    app.update_hotkey()
    assert [state["quick_capture_action"] for state in applied] == ["copy_pin", "pin"]
    assert applied[-1]["quick_capture_modifier_1"] == "ctrl"
    app.hotkey_system.register_hotkey.assert_not_called()


@pytest.mark.parametrize("disabled", [True, False])
def test_global_hotkey_toggle_refreshes_quick_capture_immediately(app, disabled):
    app.saved_settings["global_hotkeys_disabled"] = not disabled
    seen = []
    app.quick_capture.refresh.side_effect = lambda: seen.append(
        app.config_manager.get_app_setting("global_hotkeys_disabled")
    )
    MainApp.set_global_hotkeys_disabled(app, disabled)
    assert seen == [disabled]
    app.hotkey_system.set_suppressed.assert_called_once_with(disabled)


@pytest.mark.parametrize("wizard_fails", [False, True])
def test_wizard_suspends_quick_capture_then_refreshes_even_if_wizard_fails(app, monkeypatch, wizard_fails):
    events = []
    app.settings_window = Mock()
    app.quick_capture.suspend.side_effect = lambda: events.append("suspend")
    app.quick_capture.refresh.side_effect = lambda: events.append("refresh")

    def run_wizard():
        events.append("wizard")
        if wizard_fails:
            raise RuntimeError("Synthetic wizard failure")

    wizard = Mock()
    wizard.exec.side_effect = run_wizard
    monkeypatch.setattr("ui.welcome.WelcomeWizard", Mock(return_value=wizard))
    MainApp._on_wizard_requested(app)
    assert events == ["suspend", "wizard", "refresh"]
    app.settings_window.hide.assert_called_once()


def test_quit_closes_quick_capture_before_application_quits(app):
    events = []
    app.quick_capture.close.side_effect = lambda: events.append("close")
    app.app.quit.side_effect = lambda: events.append("quit")
    MainApp.quit_app(app)
    assert events == ["close", "quit"]


@pytest.mark.parametrize("cleanup_fails", [False, True])
def test_about_to_quit_closes_quick_capture_before_other_cleanup(app, monkeypatch, cleanup_fails):
    events = []
    app.quick_capture.close.side_effect = lambda: events.append("close")

    def cleanup_translation():
        events.append("translation")
        if cleanup_fails:
            raise RuntimeError("Synthetic cleanup failure")

    monkeypatch.setattr("translation.TranslationManager.cleanup", cleanup_translation)
    shutdown_recognition = Mock(side_effect=lambda: events.append("recognition"))
    monkeypatch.setattr("text_recognition.shutdown_recognition", shutdown_recognition)
    MainApp._on_about_to_quit(app)
    assert events == ["close", "translation", "recognition"]
    app._logger.close.assert_called_once()


def test_normal_capture_cannot_steal_focus_during_a_quick_capture(app, qapp):
    existing_capture = Mock()
    existing_capture._session_active = True
    app.screenshot_window = existing_capture
    app.quick_capture.busy = True
    MainApp.start_screenshot(app)
    existing_capture.activateWindow.assert_not_called()
    existing_capture.raise_.assert_not_called()
    assert app._capture_thread is None

    app.quick_capture.busy = False
    MainApp.start_screenshot(app)
    existing_capture.activateWindow.assert_called_once()
    existing_capture.raise_.assert_called_once()


@pytest.mark.parametrize("reuse", [False, True])
def test_failed_normal_capture_window_build_releases_input_block(app, monkeypatch, reuse):
    app._activate_blocking_modal = Mock(return_value=False)
    failure = RuntimeError("Synthetic window build failure")
    if reuse:
        app.screenshot_window = Mock()
        app.screenshot_window.prepare_new_session.side_effect = failure
    else:
        monkeypatch.setattr("ui.screenshot_window.ScreenshotWindow", Mock(side_effect=failure))
    with pytest.raises(RuntimeError, match="Synthetic window build failure"):
        MainApp._on_capture_ready(app, object(), object())
    assert [call.args for call in app.quick_capture.set_capture_pending.call_args_list] == [(True,), (False,)]
