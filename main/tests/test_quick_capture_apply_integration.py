"""Settings Apply reaches real quick-capture dispatch without native hooks or desktop data."""

import threading
from types import SimpleNamespace
from unittest.mock import Mock

import pytest
from PySide6.QtCore import QObject, QRect, QRectF, QThread, Qt
from PySide6.QtGui import QImage
from PySide6.QtWidgets import QDialog, QWidget

from capture import quick_capture_controller as capture_module
from core import quick_capture_input as input_module
from main_app import MainApp
from settings.tool_settings import ToolSettingsManager
from ui.settings_ui.dialog import SettingsDialog


class AppHarness(QObject):
    """Real settings signal handler and hotkey update; no MainApp startup side effects."""

    on_settings_accepted = MainApp.on_settings_accepted
    update_hotkey = MainApp.update_hotkey

    def __init__(self, config):
        super().__init__()
        self.config_manager = config
        self.hotkey_system = Mock()
        self.hotkey_system.register_hotkey.return_value = True
        self.screenshot_window = self.clipboard_window = self.clipboard_manager = None
        self.settings_window = None
        self._capture_thread = None
        self.tray_icon = Mock()
        self.start_screenshot = Mock()
        self.open_clipboard_window = Mock()
        self.pin_clipboard_image = Mock()
        self.smart_translation_controller = SimpleNamespace(trigger=Mock())
        self.set_clipboard_monitoring_enabled = Mock()
        self._show_hotkey_error = Mock()
        self.quick_capture = capture_module.QuickCaptureController(self)


class SyntheticHooks:
    def __init__(self, mouse, keyboard, failure):
        self.mouse, self.keyboard, self.failure = mouse, keyboard, failure
        self.started = self.stopped = False

    def start(self):
        self.started = True

    def stop(self):
        self.stopped = True


class SyntheticDesktop:
    """The real magnifier worker receives deterministic pixels, never desktop data."""

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        pass

    def grab(self, region):
        width, height = region["width"], region["height"]
        return SimpleNamespace(width=width, height=height, bgra=b"\x67\x45\x23\xff" * width * height)


def synthetic_image(rect):
    image = QImage(rect.width(), rect.height(), QImage.Format.Format_RGB32)
    image.fill(0xff234567)
    return image


@pytest.fixture
def integration(qapp, qtbot, tmp_settings, tmp_path, monkeypatch):
    keys, hooks = set(), []

    def create_hooks(*callbacks):
        backend = SyntheticHooks(*callbacks)
        hooks.append(backend)
        return backend

    # Keep the production modifier matching and input state machine. Only the
    # calls that talk to Windows are replaced, including menu-mask SendInput.
    native = SimpleNamespace(
        GetAsyncKeyState=lambda key: 0x8000 if key in keys else 0,
        SendInput=lambda count, _inputs, _size: count,
    )
    monkeypatch.setattr(input_module, "_Win32Hooks", create_hooks)
    monkeypatch.setattr(input_module, "_user32", lambda: native)
    monkeypatch.setattr("mss.mss", SyntheticDesktop)
    monkeypatch.setattr(capture_module, "desktop_bounds", lambda: QRect(0, 0, 800, 600))
    monkeypatch.setattr(capture_module, "flush_desktop", lambda: None)
    monkeypatch.setattr(capture_module.CaptureService, "capture_region", lambda _self, rect: synthetic_image(rect))
    monkeypatch.setattr(capture_module.CaptureService, "capture_all_screens", lambda _self: (
        synthetic_image(QRect(0, 0, 800, 600)), QRectF(0, 0, 800, 600),
    ))
    monkeypatch.setattr("ui.quick_capture_overlay.set_window_exclude_from_capture", Mock())
    copied, pinned = Mock(), Mock()
    monkeypatch.setattr(capture_module, "deliver_image_async", copied)
    monkeypatch.setattr(capture_module, "set_last_region", Mock())
    monkeypatch.setattr("pin.pin_manager.PinManager.instance", lambda: SimpleNamespace(create_pin=pinned))
    monkeypatch.setattr("ui.settings_ui.dialog.validate_global_hotkey_edits", lambda *_a, **_kw: True)
    for name in ("log_info", "log_debug", "log_warning"):
        monkeypatch.setattr(f"main_app.{name}", Mock())

    config = ToolSettingsManager(qsettings=tmp_settings)
    config.set_log_dir(str(tmp_path))
    config.set_screenshot_save_path(str(tmp_path / "captures"))
    config.set_clipboard_enabled(False)
    app = AppHarness(config)
    dialog = SettingsDialog(config, current_hotkey=config.get_hotkey())
    for attr in ("log_toggle", "autostart_toggle", "language_combo"):
        delattr(dialog, attr)
    app.settings_window = dialog
    dialog.settings_applied.connect(app.on_settings_accepted)
    dialog.show()
    qapp.processEvents()
    app.update_hotkey()
    yield SimpleNamespace(app=app, dialog=dialog, config=config, keys=keys, hooks=hooks,
                          copied=copied, pinned=pinned, qtbot=qtbot, qapp=qapp)
    app.quick_capture.close()
    dialog._skip_unsaved_close_prompt = True
    dialog.close()
    dialog.deleteLater()
    qapp.processEvents()
    app.deleteLater()


def choose(fixture, key, value):
    control = fixture.dialog._behavior_controls[f"quick_capture_{key}"]
    index = control.findData(value)
    assert index >= 0
    control.setCurrentIndex(index)


def apply(fixture):
    # The production button invokes apply_settings and emits settings_applied.
    fixture.dialog._footer_ok_btn.click()
    fixture.qapp.processEvents()
    assert fixture.dialog.isVisible()
    assert not fixture.dialog._has_unsaved_changes()


def drag(fixture, modifier_keys, *, accepted):
    fixture.keys.clear()
    fixture.keys.update(modifier_keys)
    backend = fixture.hooks[-1]

    def mouse(message, x, y):
        return backend.mouse(message, SimpleNamespace(pt=SimpleNamespace(x=x, y=y), flags=0))

    before = fixture.copied.call_count
    assert bool(mouse(input_module.WM_LBUTTONDOWN, 100, 120)) is accepted
    fixture.qapp.processEvents()
    if accepted:
        fixture.qtbot.waitUntil(lambda: fixture.app.quick_capture.overlay is not None
                               and fixture.app.quick_capture.overlay.isVisible())
    assert not mouse(input_module.WM_MOUSEMOVE, 220, 200)
    assert bool(mouse(input_module.WM_LBUTTONUP, 220, 200)) is accepted
    fixture.qapp.processEvents()
    if accepted:
        fixture.qtbot.waitUntil(lambda: fixture.copied.call_count == before + 1)
        fixture.qtbot.waitUntil(lambda: not fixture.app.quick_capture.busy)
        assert fixture.copied.call_args.args[0].size().width() == 120
        assert fixture.copied.call_args.args[0].size().height() == 80
        assert not fixture.app.quick_capture.overlay.isVisible()
    else:
        assert fixture.copied.call_count == before
        assert not fixture.app.quick_capture.busy
    # A complete gesture includes releasing its modifiers after the mouse.
    # The hook must remain alive until this release can mask Win/Alt menus.
    for key in reversed(modifier_keys):
        message = input_module.WM_SYSKEYUP if key in (0xA4, 0xA5) else input_module.WM_KEYUP
        assert not backend.keyboard(message, SimpleNamespace(vkCode=key, flags=0))
        fixture.keys.remove(key)
    fixture.qapp.processEvents()
    assert not fixture.app.quick_capture.input._pending_menu_keys


def matching_ctrl_click(fixture):
    fixture.keys.add(0xA2)
    backend = fixture.hooks[-1]
    data = SimpleNamespace(pt=SimpleNamespace(x=100, y=120), flags=0)
    return (backend.mouse(input_module.WM_LBUTTONDOWN, data),
            backend.mouse(input_module.WM_LBUTTONUP, data))


def configure_ctrl(fixture):
    fixture.config.set_app_setting("quick_capture_modifier_1", "ctrl")
    fixture.config.set_app_setting("quick_capture_modifier_2", "")
    fixture.app.quick_capture.refresh()


def test_normal_capture_show_passes_ctrl_click_and_hide_restores_quick_capture(integration):
    f = integration
    configure_ctrl(f)
    window = QWidget()
    f.qtbot.addWidget(window)
    window._session_active = True
    f.app.screenshot_window = window
    window.show()
    # No event-loop wait: the Show filter must update native admission first.
    assert matching_ctrl_click(f) == (False, False)
    f.qapp.processEvents()
    assert not f.app.quick_capture.busy
    assert not f.copied.called
    window._session_active = False
    window.hide()
    drag(f, [0xA2], accepted=True)


def test_modal_show_passes_ctrl_click_and_hide_restores_quick_capture(integration):
    f = integration
    configure_ctrl(f)
    modal = QDialog()
    f.qtbot.addWidget(modal)
    modal.setWindowModality(Qt.WindowModality.ApplicationModal)
    modal.show()
    assert matching_ctrl_click(f) == (False, False)
    f.qapp.processEvents()
    assert not f.app.quick_capture.busy
    modal.hide()
    drag(f, [0xA2], accepted=True)


@pytest.mark.parametrize("reuse", [False, True])
def test_normal_capture_build_blocks_input_before_first_show_or_reuse(integration, monkeypatch, reuse):
    f = integration
    configure_ctrl(f)
    window = QWidget()
    window._session_active = False
    f.qtbot.addWidget(window)
    f.app._activate_blocking_modal = lambda: False
    observed = []

    def prepare(*_args, **_kwargs):
        observed.append(matching_ctrl_click(f))
        window._session_active = True
        window.show()
        return window

    if reuse:
        window.prepare_new_session = prepare
        f.app.screenshot_window = window
    else:
        monkeypatch.setattr("ui.screenshot_window.ScreenshotWindow", prepare)
    MainApp._on_capture_ready(f.app, synthetic_image(QRect(0, 0, 800, 600)), QRectF(0, 0, 800, 600))
    assert observed == [(False, False)]
    assert matching_ctrl_click(f) == (False, False)
    window._session_active = False
    window.hide()
    drag(f, [0xA2], accepted=True)


@pytest.mark.parametrize("magnifier_enabled", [False, True])
def test_hook_thread_motion_burst_renders_latest_point_once_on_gui_thread(
    integration, monkeypatch, magnifier_enabled,
):
    fixture = integration
    controller = fixture.app.quick_capture
    fixture.config.set_app_setting("magnifier_enabled", magnifier_enabled)
    fixture.keys.add(0x5B)
    backend = fixture.hooks[-1]

    def mouse(message, x, y):
        return backend.mouse(message, SimpleNamespace(pt=SimpleNamespace(x=x, y=y), flags=0))

    assert mouse(input_module.WM_LBUTTONDOWN, 100, 120)
    fixture.qapp.processEvents()
    overlay = controller.overlay
    painted_threads = []
    show_selection = overlay.show_selection

    def record_selection(*args):
        painted_threads.append(QThread.currentThread())
        show_selection(*args)

    monkeypatch.setattr(overlay, "show_selection", record_selection)

    def burst():
        for offset in range(500):
            mouse(input_module.WM_MOUSEMOVE, 101 + offset, 121 + offset // 2)

    producer = threading.Thread(target=burst)
    producer.start()
    producer.join(timeout=2)
    assert not producer.is_alive()
    fixture.qapp.processEvents()
    assert painted_threads == [fixture.qapp.thread()]
    assert overlay.model.rect() == QRectF(100, 120, 500, 250)
    assert controller.input.take_position(controller._active) is None
    assert (controller._preview is not None) is magnifier_enabled
    # Final capture uses the release position even if no move event announced it.
    assert mouse(input_module.WM_LBUTTONUP, 620, 380)
    fixture.qtbot.waitUntil(lambda: fixture.copied.called)
    assert fixture.copied.call_args.args[0].size().width() == 520
    assert fixture.copied.call_args.args[0].size().height() == 260
    assert not overlay.isVisible()


def test_apply_switches_real_dispatch_from_win_to_ctrl_alt_without_closing_settings(integration):
    fixture = integration
    drag(fixture, [0x5B], accepted=True)
    assert fixture.pinned.call_count == 1

    choose(fixture, "modifier_1", "ctrl")
    choose(fixture, "modifier_2", "alt")
    choose(fixture, "action", "copy")
    apply(fixture)
    assert fixture.config.get_app_setting("quick_capture_modifier_1") == "ctrl"
    assert fixture.config.get_app_setting("quick_capture_modifier_2") == "alt"
    drag(fixture, [0x5B], accepted=False)
    drag(fixture, [0xA2], accepted=False)
    drag(fixture, [0xA2, 0xA4], accepted=True)
    assert fixture.pinned.call_count == 1
    fixture.app.set_clipboard_monitoring_enabled.assert_called_once_with(False)


def test_apply_disabled_action_and_restore_defaults_reconfigure_real_input(integration):
    fixture = integration
    choose(fixture, "modifier_1", "ctrl")
    choose(fixture, "modifier_2", "alt")
    choose(fixture, "action", "copy")
    apply(fixture)
    drag(fixture, [0xA2, 0xA4], accepted=True)
    choose(fixture, "action", "none")
    apply(fixture)
    assert fixture.hooks[-1].stopped
    drag(fixture, [0xA2, 0xA4], accepted=False)

    fixture.dialog._reset_hotkey_page()
    apply(fixture)
    assert fixture.config.get_app_setting("quick_capture_action") == "copy_pin"
    assert fixture.hooks[-1].started and not fixture.hooks[-1].stopped
    drag(fixture, [0xA2, 0xA4], accepted=False)
    drag(fixture, [0x5B], accepted=True)
    assert fixture.pinned.call_count == 1
