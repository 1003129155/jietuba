"""Capture actions, pin mouse bindings and their settings integration."""

from pathlib import Path
from types import SimpleNamespace
from unittest.mock import MagicMock

import pytest
from PySide6.QtCore import QEvent, QPoint, QPointF, QRectF, QSettings, Qt
from PySide6.QtGui import QCloseEvent, QImage, QKeyEvent, QMouseEvent, QWheelEvent
from PySide6.QtWidgets import QApplication, QWidget

from settings.tool_settings import CAPTURE_TRIGGERS, ToolSettingsManager, get_capture_action
from tools.action import ActionTools
from ui.settings_ui.dialog import SettingsDialog


@pytest.fixture
def config(tmp_path):
    manager = ToolSettingsManager(qsettings=QSettings(str(tmp_path / 'settings.ini'), QSettings.IniFormat))
    manager.set_log_dir(str(tmp_path))
    manager.set_screenshot_save_path(str(tmp_path / 'captures'))
    manager.set_app_setting('ocr_enabled', False)
    manager.set_app_setting('pin_auto_toolbar', False)
    manager.set_smart_selection(False)
    return manager


@pytest.fixture
def settings(qapp, config, monkeypatch):
    monkeypatch.setattr('ui.settings_ui.dialog.validate_global_hotkey_edits', lambda *_a, **_kw: True)
    dialog = SettingsDialog(config)
    for attr in ('log_toggle', 'autostart_toggle', 'language_combo'):
        delattr(dialog, attr)
    dialog.show()
    qapp.processEvents()
    yield dialog
    dialog._skip_unsaved_close_prompt = True
    dialog.close()
    dialog.deleteLater()


def choose(dialog, key, value):
    control = dialog._behavior_controls[key]
    if hasattr(control, 'setBinding'):
        control.setBinding(value)
    elif hasattr(control, 'findData'):
        control.setCurrentIndex(control.findData(value))
    else:
        control.setChecked(value)


def test_apply_saves_without_closing_and_resets_dirty_state(settings, config, qapp):
    applied = []
    accepted = []
    settings.settings_applied.connect(lambda: applied.append(True))
    settings.accepted.connect(lambda: accepted.append(True))
    choose(settings, 'capture_fullscreen_crosshair', True)
    choose(settings, 'capture_enter_action', 'quick_save')
    choose(settings, 'capture_enter_exit', False)
    choose(settings, 'mouse_pin_close', 'ctrl+doublemiddle')
    assert settings._has_unsaved_changes()
    settings._footer_ok_btn.click()
    qapp.processEvents()
    assert settings.isVisible()
    assert applied == [True] and not accepted
    assert config.get_app_setting('capture_fullscreen_crosshair') is True
    assert config.get_app_setting('capture_enter_action') == 'quick_save'
    assert config.get_app_setting('mouse_pin_close') == 'ctrl+doublemiddle'
    assert not settings._has_unsaved_changes()
    choose(settings, 'capture_fullscreen_crosshair', False)
    assert settings._has_unsaved_changes()
    settings.apply_settings()
    assert applied == [True, True] and settings.isVisible()


def test_reset_refresh_all_new_controls(settings, config):
    choose(settings, 'capture_enter_action', 'none')
    assert not settings._behavior_controls['capture_enter_exit'].isEnabled()
    settings._reset_quick_actions_page()
    assert settings._behavior_controls['capture_enter_action'].currentData() == 'copy'
    assert settings._behavior_controls['capture_enter_exit'].isEnabled()
    choose(settings, 'mouse_pin_close', 'ctrl+right')
    settings._reset_hotkey_page()
    assert settings._behavior_controls['mouse_pin_close'].currentData() == 'doubleleft'
    config.set_app_setting('capture_enter_action', 'pin')
    config.set_app_setting('mouse_pin_opacity', 'alt+wheel')
    settings.refresh_settings()
    assert settings._behavior_controls['capture_enter_action'].currentData() == 'pin'
    assert settings._behavior_controls['mouse_pin_opacity'].currentData() == 'alt+wheel'


def test_invalid_apply_and_save_on_close_keep_window_open(settings, monkeypatch):
    monkeypatch.setattr('ui.settings_ui.dialog.validate_global_hotkey_edits', lambda *_a, **_kw: False)
    monkeypatch.setattr('ui.settings_ui.dialog.show_warning_dialog', lambda *_a, **_kw: None)
    monkeypatch.setattr(settings, '_confirm_close_with_unsaved_changes', lambda: 'save')
    choose(settings, 'capture_fullscreen_crosshair', True)
    assert settings.apply_settings() is False
    event = QCloseEvent()
    settings.closeEvent(event)
    assert not event.isAccepted()
    assert settings.isVisible()


@pytest.mark.parametrize('trigger,_label,default', CAPTURE_TRIGGERS)
def test_capture_action_persistence_and_legacy(config, trigger, _label, default):
    assert get_capture_action(config, trigger) == default
    if trigger == 'double_click':
        config.set_double_click_copy_close_enabled(False)
        assert get_capture_action(config, trigger) == 'none'
    config.set_app_setting(f'capture_{trigger}_action', 'save')
    assert get_capture_action(config, trigger) == 'save'


@pytest.fixture
def actions(config, monkeypatch):
    scene = MagicMock()
    scene.selection_model.is_confirmed = True
    scene.selection_model.rect.return_value = QRectF(0, 0, 40, 30)
    window = MagicMock()
    tools = ActionTools(scene, config, window)
    tools._temporarily_exit_editing = MagicMock()
    image = QImage(40, 30, QImage.Format_ARGB32)
    image.fill(Qt.red)
    tools.export_service.export = MagicMock(return_value=image)
    monkeypatch.setattr('core.clipboard_utils.deliver_image_async', MagicMock())
    return tools


@pytest.mark.parametrize('close', [False, True])
@pytest.mark.parametrize('trigger,_label,_default', CAPTURE_TRIGGERS)
def test_copy_action_respects_close_flag(actions, config, close, trigger, _label, _default):
    config.set_screenshot_save_enabled(False)
    config.set_app_setting(f'capture_{trigger}_action', 'copy')
    config.set_app_setting(f'capture_{trigger}_exit', close)
    assert actions.handle_capture_action(trigger)
    assert actions.parent_window.hide.called is close
    assert actions.parent_window.cleanup_and_close.called is close


def test_none_and_unconfirmed_do_nothing(actions, config):
    config.set_app_setting('capture_enter_action', 'none')
    assert actions.handle_capture_action('enter') is False
    actions.export_service.export.assert_not_called()
    actions.scene.selection_model.is_confirmed = False
    config.set_app_setting('capture_enter_action', 'copy')
    assert actions.handle_capture_action('enter') is False


@pytest.mark.parametrize('close', [False, True])
def test_quick_save_writes_file_even_when_auto_save_disabled(actions, config, close):
    config.set_screenshot_save_enabled(False)
    config.set_app_setting('capture_enter_action', 'quick_save')
    config.set_app_setting('capture_enter_exit', close)
    actions.handle_capture_action('enter')
    files = list(Path(config.get_screenshot_save_path()).glob('*'))
    assert len(files) == 1
    assert not QImage(str(files[0])).isNull()
    assert actions.parent_window.cleanup_and_close.called is close


def test_save_cancel_or_failure_preserves_capture(actions, config, monkeypatch, tmp_path):
    config.set_app_setting('capture_enter_action', 'save')
    monkeypatch.setattr('tools.action.QFileDialog.getSaveFileName', lambda *_a: ('', ''))
    actions.handle_capture_action('enter')
    actions.parent_window.cleanup_and_close.assert_not_called()
    monkeypatch.setattr('tools.action.QFileDialog.getSaveFileName', lambda *_a: (str(tmp_path / 'out.png'), 'PNG (*.png)'))
    monkeypatch.setattr('ui.dialogs.show_warning_dialog', lambda *_a: None)
    actions.save_service.save_qimage_to_path = MagicMock(return_value=False)
    actions.handle_capture_action('enter')
    actions.parent_window.cleanup_and_close.assert_not_called()


@pytest.fixture
def pin(qapp, config, monkeypatch):
    from pin.pin_window import PinWindow
    from pin.pin_ocr_manager import PinOCRManager
    monkeypatch.setattr(PinOCRManager, 'init_now', lambda self: None)
    image = QImage(400, 300, QImage.Format_ARGB32)
    image.fill(Qt.green)
    window = PinWindow(image, QPoint(120, 120), config)
    qapp.processEvents()
    yield window
    if not window._is_closed:
        window.close_window()


def mouse_event(pin, kind, button, point=(30, 30), modifiers=Qt.NoModifier):
    local = QPointF(*point)
    global_pos = QPointF(pin.view.viewport().mapToGlobal(local.toPoint()))
    return QMouseEvent(kind, local, global_pos, button, button, modifiers)


def gesture(pin, button, modifiers=Qt.NoModifier):
    for kind in (QEvent.MouseButtonPress, QEvent.MouseButtonRelease):
        QApplication.sendEvent(pin.view.viewport(), mouse_event(pin, kind, button, modifiers=modifiers))


def test_mouse_reset_change_and_disable_apply_to_existing_pin(pin, config, monkeypatch):
    reset = MagicMock()
    monkeypatch.setattr(pin, 'reset_to_original_size', reset)
    gesture(pin, Qt.MiddleButton)
    reset.assert_called_once()
    config.set_app_setting('mouse_pin_reset', 'ctrl+middle')
    gesture(pin, Qt.MiddleButton)
    assert reset.call_count == 1
    gesture(pin, Qt.MiddleButton, Qt.ControlModifier)
    assert reset.call_count == 2
    config.set_app_setting('mouse_pin_reset', '')
    gesture(pin, Qt.MiddleButton, Qt.ControlModifier)
    assert reset.call_count == 2


def test_wheel_modifiers_and_keyboard_alternatives(pin, config):
    def wheel(modifiers, delta=120):
        event = QWheelEvent(QPointF(), QPointF(), QPoint(), QPoint(0, delta),
                            Qt.NoButton, modifiers, Qt.NoScrollPhase, False)
        pin.wheelEvent(event)
    wheel(Qt.NoModifier)
    assert pin.scale_factor == pytest.approx(1.05)
    wheel(Qt.ControlModifier, -120)
    assert pin._win_opacity == pytest.approx(.95)
    config.set_app_setting('mouse_pin_zoom', 'alt+wheel')
    wheel(Qt.NoModifier)
    assert pin.scale_factor == pytest.approx(1.05)
    wheel(Qt.AltModifier)
    assert pin.scale_factor > 1.05
    key = QKeyEvent(QEvent.KeyPress, Qt.Key_Minus, Qt.ControlModifier)
    assert pin.handle_mouse_alternative_key(key)
    assert pin._win_opacity == pytest.approx(.90)
    config.set_app_setting('mouse_pin_opacity_keys', False)
    assert pin.handle_mouse_alternative_key(key)
    assert pin._win_opacity == pytest.approx(.85)


def test_region_drag_creates_reversible_thumbnail(pin, qapp):
    original = pin.size()
    QApplication.sendEvent(pin.view.viewport(), mouse_event(pin, QEvent.MouseButtonPress, Qt.RightButton, (30, 40)))
    QApplication.sendEvent(pin.view.viewport(), mouse_event(pin, QEvent.MouseMove, Qt.NoButton, (180, 160)))
    QApplication.sendEvent(pin.view.viewport(), mouse_event(pin, QEvent.MouseButtonRelease, Qt.RightButton, (180, 160)))
    assert pin._thumbnail_mode
    assert pin._thumbnail._region_rect.width() > 100
    assert pin.width() < original.width()
    pin.toggle_thumbnail_mode()
    assert pin.size() == original
    assert not pin._thumbnail_mode


def test_default_thumbnail_double_click_and_editing_guard(pin, monkeypatch):
    toggle = MagicMock()
    monkeypatch.setattr(pin, 'toggle_thumbnail_mode', toggle)
    event = mouse_event(pin, QEvent.MouseButtonDblClick, Qt.LeftButton, modifiers=Qt.ShiftModifier)
    QApplication.sendEvent(pin.view.viewport(), event)
    toggle.assert_called_once()
    pin.canvas.activate_tool('pen')
    QApplication.sendEvent(pin.view.viewport(), event)
    assert toggle.call_count == 1


def test_copy_text_and_right_click_menu(pin, monkeypatch):
    copy = MagicMock()
    menu = MagicMock()
    layer = SimpleNamespace(get_selected_text=lambda: 'selected', _copy_selected_text=copy)
    pin._ocr_mgr.ocr_text_layer = layer
    monkeypatch.setattr(pin, 'show_context_menu', menu)
    gesture(pin, Qt.RightButton)
    copy.assert_called_once()
    menu.assert_not_called()
    layer.get_selected_text = lambda: ''
    gesture(pin, Qt.RightButton)
    menu.assert_called_once()
    pin._ocr_mgr.ocr_text_layer = None


@pytest.mark.parametrize("region_binding", ["", "dragright"])
def test_right_double_click_defers_context_menu(pin, config, monkeypatch, region_binding):
    config.set_app_setting("mouse_pin_region", region_binding)
    config.set_app_setting('mouse_pin_close', 'doubleright')
    menu = MagicMock()
    close = MagicMock()
    monkeypatch.setattr(pin, 'show_context_menu', menu)
    monkeypatch.setattr(pin, 'close_window', close)
    gesture(pin, Qt.RightButton)
    menu.assert_not_called()
    QApplication.sendEvent(pin.view.viewport(), mouse_event(pin, QEvent.MouseButtonDblClick, Qt.RightButton))
    close.assert_called_once()
    assert not pin._mouse_click_timer.isActive()


def test_keyboard_zoom_stays_enabled_with_legacy_disabled_setting(pin, config):
    config.set_app_setting('mouse_pin_zoom', 'ctrl+wheel')
    config.set_app_setting('mouse_pin_zoom_keys', False)
    key = QKeyEvent(QEvent.KeyPress, Qt.Key_Minus, Qt.ControlModifier)
    assert pin.handle_mouse_alternative_key(key)
    assert pin._win_opacity == 1.0
    assert pin.scale_factor == pytest.approx(1 / 1.05)


def test_plus_preserves_explicit_shift_binding(pin, config):
    config.set_app_setting('mouse_pin_zoom', 'shift+wheel')
    key = QKeyEvent(QEvent.KeyPress, Qt.Key_Plus, Qt.ShiftModifier)
    assert pin.handle_mouse_alternative_key(key)
    assert pin.scale_factor == pytest.approx(1.05)


def test_screenshot_reuse_rebinds_crosshair_and_capture_actions(config, qapp, monkeypatch):
    from ui.screenshot_window import ScreenshotWindow
    from pin.pin_manager import PinManager
    monkeypatch.setattr(PinManager, 'suppress_topmost', lambda self: None)
    monkeypatch.setattr(PinManager, 'restore_topmost', lambda self: None)
    image = QImage(400, 300, QImage.Format_ARGB32)
    image.fill(Qt.blue)
    config.set_app_setting('capture_fullscreen_crosshair', True)
    window = ScreenshotWindow(config, prefetched_image=image, prefetched_rect=QRectF(-200, 0, 400, 300))
    qapp.processEvents()
    assert window.view._fullscreen_crosshair
    assert window.view._crosshair_surface is window.mask_overlay
    old_viewport = window.view.viewport()
    window.cleanup_and_close()
    assert window.mask_overlay._crosshair_position is None
    config.set_app_setting('capture_fullscreen_crosshair', False)
    config.set_app_setting('capture_double_click_action', 'none')
    window.prepare_new_session(image, QRectF(0, 0, 400, 300))
    assert window.view.viewport() is not old_viewport
    assert not window.view._fullscreen_crosshair
    assert not window.view.confirm_on_double_click
    window.cleanup_and_close()
    window.deleteLater()


def test_crosshair_replaces_cursor_and_keeps_export_clean(qapp):
    from ui.mask_overlay import MaskOverlayWidget
    from canvas import CanvasScene, CanvasView
    from core.export import ExportService
    parent = QWidget()
    parent.setGeometry(-200, 50, 320, 240)
    image = QImage(320, 240, QImage.Format_ARGB32)
    image.fill(Qt.blue)
    scene = CanvasScene(image, QRectF(-200, 50, 320, 240), enable_mosaic=False)
    view = CanvasView(scene, parent)
    view.setGeometry(parent.rect())
    mask = MaskOverlayWidget(parent, scene.selection_model)
    mask.setGeometry(parent.rect())
    parent.show()
    qapp.processEvents()
    view.set_fullscreen_crosshair(True, mask)
    view.setCursor(Qt.CrossCursor)
    viewport = view.viewport()
    position = QPoint(140, 100)
    move = QMouseEvent(QEvent.MouseMove, QPointF(position),
                       QPointF(viewport.mapToGlobal(position)), Qt.NoButton, Qt.NoButton, Qt.NoModifier)
    QApplication.sendEvent(viewport, move)
    # There is no native cross left to run ahead of the fullscreen cursor.
    assert viewport.cursor().shape() == Qt.BlankCursor
    assert mask._crosshair_position == mask.mapFromGlobal(viewport.mapToGlobal(position))
    rendered = mask.grab().toImage()
    assert rendered.pixelColor(10, position.y()).lightness() > 100
    assert rendered.pixelColor(position.x(), 10).lightness() > 100
    exported = ExportService(scene).export(QRectF(-200, 50, 320, 240))
    assert exported.pixelColor(140, 100) == image.pixelColor(140, 100)
    # Item-driven cursor changes must also hide the fullscreen cursor.
    for shape in (Qt.IBeamCursor, Qt.SizeAllCursor, Qt.SizeHorCursor):
        viewport.setCursor(shape)
        assert mask._crosshair_position is None
        assert viewport.cursor().shape() == shape
    view.setCursor(Qt.CrossCursor)
    assert mask._crosshair_position is not None
    QApplication.sendEvent(viewport, QEvent(QEvent.Leave))
    assert mask._crosshair_position is None
    view.set_fullscreen_crosshair(False)
    assert viewport.cursor().shape() == Qt.CrossCursor
    view.cleanup()
    parent.close()
    parent.deleteLater()
    scene.deleteLater()


def test_capture_actions_follow_text_top_with_inline_switches(settings, qapp):
    from ui.fluent_lite import SwitchButton
    settings._on_nav_changed(9, 'quick_actions')
    qapp.processEvents()
    previous_y = settings.text_always_on_top_toggle.mapTo(settings, QPoint()).y()
    for trigger, _label, _default in CAPTURE_TRIGGERS:
        combo = settings._behavior_controls[f'capture_{trigger}_action']
        switch = settings._behavior_controls[f'capture_{trigger}_exit']
        assert isinstance(switch, SwitchButton)
        combo_center = combo.mapTo(settings, combo.rect().center())
        switch_center = switch.mapTo(settings, switch.rect().center())
        assert abs(combo_center.y() - switch_center.y()) <= 1
        assert combo_center.y() > previous_y
        assert switch_center.x() > combo_center.x()
        previous_y = combo_center.y()


def test_right_single_click_keeps_menu_available_with_double_click_binding(pin, config, monkeypatch, qtbot):
    config.set_app_setting('mouse_pin_region', '')
    config.set_app_setting('mouse_pin_close', 'doubleright')
    menu = MagicMock()
    monkeypatch.setattr(pin, 'show_context_menu', menu)
    interval = QApplication.doubleClickInterval()
    QApplication.setDoubleClickInterval(20)
    try:
        gesture(pin, Qt.RightButton)
        menu.assert_not_called()
        qtbot.waitUntil(lambda: menu.call_count == 1)
    finally:
        QApplication.setDoubleClickInterval(interval)


def test_save_on_close_does_not_reopen_scaled_settings(settings, config, monkeypatch):
    from main_app import MainApp
    from core.ui_scale import get_dialog_scale
    reopen = MagicMock()
    app = SimpleNamespace(
        sender=lambda: settings,
        config_manager=config,
        set_clipboard_monitoring_enabled=lambda _enabled: None,
        update_hotkey=lambda **_kwargs: None,
        _recreate_clipboard_manage_dialog=lambda: None,
        _recreate_settings_window=reopen,
    )
    settings.settings_applied.connect(lambda: MainApp.on_settings_accepted(app))
    before = get_dialog_scale().percent
    combo = settings._dialog_scale_combo
    index = next(i for i in range(combo.count()) if combo.itemData(i) != before)
    combo.setCurrentIndex(index)
    monkeypatch.setattr(settings, '_confirm_close_with_unsaved_changes', lambda: 'save')
    try:
        assert settings.close()
        reopen.assert_called_once_with(reopen=False)
    finally:
        get_dialog_scale().set_percent(before)
