"""Exercise row gestures through Qt without accessing the real clipboard."""

from unittest.mock import Mock
from types import SimpleNamespace

import pytest
from PySide6.QtCore import QEvent, QPoint, QPointF, QSettings, Qt
from PySide6.QtGui import QContextMenuEvent, QMouseEvent, QKeyEvent
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication, QListWidget, QListWidgetItem, QWidget, QStackedWidget

from clipboard.controllers.mouse_shortcut_controller import ClipboardMouseController
from clipboard.controllers.selection_manager import SelectionManager
from clipboard.core import ClipboardItem
from clipboard.ui.windows.clipboard_window import ClipboardShortcutHandler
from settings.tool_settings import CLIPBOARD_MOUSE_ACTIONS, ToolSettingsManager
from ui.settings_ui.dialog import SettingsDialog
from ui.settings_ui.page_hotkey import mouse_binding_conflicts


@pytest.fixture
def config(tmp_path):
    return ToolSettingsManager(qsettings=QSettings(str(tmp_path / 'mouse.ini'), QSettings.IniFormat))


@pytest.fixture
def window(qtbot, config):
    window = QWidget()
    qtbot.addWidget(window)
    window.config = config
    window.resize(320, 250)
    window.list_widget = QListWidget(window)
    window.list_widget.setGeometry(0, 0, 320, 250)
    window.items = {i: ClipboardItem(id=i, content='example', content_type=kind)
                    for i, kind in ((1, 'text'), (2, 'image'), (3, 'text'), (4, 'file'))}
    for item_id in window.items:
        row = QListWidgetItem(str(item_id), window.list_widget)
        row.setData(Qt.UserRole, item_id)
    window._get_item_data = window.items.get
    window._on_paste_item = Mock()
    window._create_pin_window = Mock()
    window._quick_edit_item = Mock()
    window._show_item_context_menu = Mock()
    window.selection = SelectionManager(window.list_widget, window.items.get)
    window.selection.item_activated.connect(window._on_paste_item)
    window.mouse = ClipboardMouseController(window)
    window.show()
    window.activateWindow()
    QApplication.processEvents()
    previous_interval = QApplication.doubleClickInterval()
    QApplication.setDoubleClickInterval(80)
    yield window
    window.mouse.cancel()
    QApplication.setDoubleClickInterval(previous_interval)
    window.selection.deleteLater()


def point(window, row=0):
    return window.list_widget.visualItemRect(window.list_widget.item(row)).center()


def click(window, row=0, button=Qt.LeftButton, modifiers=Qt.NoModifier):
    QTest.mouseClick(window.list_widget.viewport(), button, modifiers, point(window, row))


def double_click(window, row=0, button=Qt.LeftButton, modifiers=Qt.NoModifier):
    click(window, row, button, modifiers)
    QTest.mouseDClick(window.list_widget.viewport(), button, modifiers, point(window, row))
    QTest.mouseRelease(window.list_widget.viewport(), button, modifiers, point(window, row))


def test_single_text_click_waits_then_pastes_once(window, qtbot):
    window.config.set_app_setting('mouse_clipboard_quick_edit', 'doubleleft')
    click(window)
    window._on_paste_item.assert_not_called()
    qtbot.waitUntil(lambda: window._on_paste_item.call_count == 1)
    window._on_paste_item.assert_called_once_with(1)


def test_double_text_click_edits_without_pasting(window, qtbot):
    window.config.set_app_setting('mouse_clipboard_quick_edit', 'doubleleft')
    double_click(window)
    window._quick_edit_item.assert_called_once_with(1)
    qtbot.wait(120)
    window._on_paste_item.assert_not_called()


@pytest.mark.parametrize('row', [0, 1, 3])
def test_default_left_click_pastes_immediately(window, row):
    click(window, row)
    window._on_paste_item.assert_called_once_with(row + 1)


def test_disabling_double_click_makes_text_paste_immediate(window):
    window.config.set_app_setting('mouse_clipboard_quick_edit', '')
    click(window)
    window._on_paste_item.assert_called_once_with(1)


def test_middle_edits_text_pins_images_and_ignores_files(window):
    click(window, 0, Qt.MiddleButton)
    click(window, 1, Qt.MiddleButton)
    click(window, 3, Qt.MiddleButton)
    window._quick_edit_item.assert_called_once_with(1)
    window._create_pin_window.assert_called_once_with(2)
    window._on_paste_item.assert_not_called()


def test_default_menu_and_remapping_suppress_native_right_click(window):
    click(window, button=Qt.RightButton)
    assert window._show_item_context_menu.call_args.args[0] == 1
    window._show_item_context_menu.reset_mock()
    window.config.set_app_setting('mouse_clipboard_menu', 'ctrl+left')
    click(window, button=Qt.RightButton)
    event = QContextMenuEvent(QContextMenuEvent.Mouse, point(window), QPoint(100, 100))
    assert window.mouse.eventFilter(window.list_widget.viewport(), event)
    window._show_item_context_menu.assert_not_called()
    click(window, modifiers=Qt.ControlModifier)
    window._show_item_context_menu.assert_called_once()
    window._on_paste_item.assert_not_called()


def test_modifiers_are_exact_and_bindings_update_without_reopening(window):
    click(window, modifiers=Qt.ControlModifier)
    window._on_paste_item.assert_not_called()
    window.config.set_app_setting('mouse_clipboard_paste', 'ctrl+middle')
    click(window, modifiers=Qt.ControlModifier, button=Qt.MiddleButton)
    window._on_paste_item.assert_called_once_with(1)
    window._create_pin_window.assert_not_called()


@pytest.mark.parametrize('cancel', ['hide', 'key', 'delete', 'reset'])
def test_pending_click_does_not_fire_after_context_changes(window, qtbot, cancel):
    window.config.set_app_setting('mouse_clipboard_quick_edit', 'doubleleft')
    click(window)
    if cancel == 'hide':
        window.hide()
    elif cancel == 'key':
        QTest.keyClick(window.list_widget, Qt.Key_Escape)
    elif cancel == 'delete':
        window.items.pop(1)
        window.list_widget.takeItem(0)
    else:
        window.list_widget.clear()
    qtbot.wait(120)
    window._on_paste_item.assert_not_called()


def test_pending_click_tracks_item_id_across_insertions(window, qtbot):
    window.config.set_app_setting('mouse_clipboard_quick_edit', 'doubleleft')
    click(window)
    row = QListWidgetItem('new')
    row.setData(Qt.UserRole, 99)
    window.list_widget.insertItem(0, row)
    qtbot.waitUntil(lambda: window._on_paste_item.called)
    window._on_paste_item.assert_called_once_with(1)


def test_global_keyboard_handler_cancels_pending_paste(window, qtbot):
    window.config.set_app_setting('mouse_clipboard_quick_edit', 'doubleleft')
    window._mouse_controller = window.mouse
    window.selection_manager = window.selection
    click(window)
    handler = ClipboardShortcutHandler(window)
    event = QKeyEvent(QEvent.KeyPress, Qt.Key_Escape, Qt.NoModifier)
    assert handler.handle_key(event)
    qtbot.wait(120)
    window._on_paste_item.assert_not_called()


def test_drag_and_empty_space_do_not_activate(window, qtbot):
    viewport = window.list_widget.viewport()
    start = point(window)
    end = start + QPoint(45, 0)
    QTest.mousePress(viewport, Qt.LeftButton, pos=start)
    move = QMouseEvent(QEvent.MouseMove, QPointF(end), QPointF(viewport.mapToGlobal(end)),
                       Qt.NoButton, Qt.LeftButton, Qt.NoModifier)
    QApplication.sendEvent(viewport, move)
    QTest.mouseRelease(viewport, Qt.LeftButton, pos=end)
    QTest.mouseClick(viewport, Qt.LeftButton, pos=QPoint(20, 200))
    qtbot.wait(120)
    window._on_paste_item.assert_not_called()


def test_double_click_on_different_row_does_not_edit_old_item(window, qtbot):
    window.config.set_app_setting('mouse_clipboard_quick_edit', 'doubleleft')
    click(window, 0)
    QTest.mouseDClick(window.list_widget.viewport(), Qt.LeftButton, pos=point(window, 2))
    QTest.mouseRelease(window.list_widget.viewport(), Qt.LeftButton, pos=point(window, 2))
    qtbot.waitUntil(lambda: window._on_paste_item.called)
    window._on_paste_item.assert_called_once_with(3)
    window._quick_edit_item.assert_not_called()


def test_right_double_click_cancels_delayed_menu(window, qtbot):
    window.config.set_app_setting('mouse_clipboard_quick_edit', 'doubleright')
    double_click(window, button=Qt.RightButton)
    window._quick_edit_item.assert_called_once_with(1)
    qtbot.wait(120)
    window._show_item_context_menu.assert_not_called()


@pytest.mark.parametrize(('binding', 'extra'), [
    (binding, extra)
    for binding in ('middle', 'ctrl+middle', 'doubleleft')
    for extra in (None, 'paste', 'menu', 'inapp')
    if (binding, extra) != ('doubleleft', 'inapp')
])
def test_only_disjoint_clipboard_actions_can_share_a_binding(binding, extra):
    controls = {f'mouse_clipboard_{action}': SimpleNamespace(currentData=lambda: binding)
                for action in ('pin', 'quick_edit')}
    if extra in ('paste', 'menu'):
        controls[f'mouse_clipboard_{extra}'] = SimpleNamespace(currentData=lambda: binding)
    dialog = SimpleNamespace(
        _behavior_controls=controls,
        _inapp_edits={}, _inapp_groups={}, tr=lambda text: text,
    )
    if extra == 'inapp':
        # Only middle-click is accepted by the existing keyboard shortcut editor.
        dialog._inapp_edits['inapp_clipboard_quick_edit'] = SimpleNamespace(
            text=lambda: binding.replace('middle', 'mousemiddle'))
        dialog._inapp_groups['inapp_clipboard_quick_edit'] = 'clipboard'
    assert bool(mouse_binding_conflicts(dialog)) is (extra is not None)


def test_clipboard_tab_persists_resets_and_validates_bindings(qtbot, config, monkeypatch):
    monkeypatch.setattr('ui.settings_ui.dialog.validate_global_hotkey_edits', lambda *_a, **_kw: True)
    warnings = Mock()
    monkeypatch.setattr('ui.settings_ui.dialog.show_warning_dialog', warnings)
    dialog = SettingsDialog(config)
    qtbot.addWidget(dialog)
    for attr in ('log_toggle', 'autostart_toggle', 'language_combo'):
        delattr(dialog, attr)
    dialog._skip_unsaved_close_prompt = True
    dialog.show()
    stack = dialog.findChild(QStackedWidget, 'MouseShortcutStack')
    assert stack.count() == 3
    stack.setCurrentIndex(2)
    for action, _label, default, _kind in CLIPBOARD_MOUSE_ACTIONS:
        control = dialog._behavior_controls[f'mouse_clipboard_{action}']
        assert control.isVisible()
        assert control.currentData() == default
    paste = dialog._behavior_controls['mouse_clipboard_paste']
    paste.setBinding('right')
    assert dialog.apply_settings() is False
    assert warnings.call_count == 1
    paste.setBinding('ctrl+left')
    assert dialog.apply_settings() is True
    assert config.get_app_setting('mouse_clipboard_paste') == 'ctrl+left'
    dialog._reset_hotkey_page()
    assert paste.currentData() == 'left'
    assert dialog._behavior_controls['mouse_clipboard_quick_edit'].currentData() == 'middle'
    assert not mouse_binding_conflicts(dialog)
    dialog.refresh_settings()
    assert paste.currentData() == 'ctrl+left'
    dialog._skip_unsaved_close_prompt = True
