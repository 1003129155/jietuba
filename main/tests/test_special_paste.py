# -*- coding: utf-8 -*-
"""特殊粘贴配置在编辑、应用、重新打开菜单之间保持一致。"""

import json

import pytest
from PySide6.QtCore import QPoint, QSettings, Qt, QTimer
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication, QDialog, QPushButton

from clipboard.controllers.clipboard_controller import ClipboardController
from clipboard.core import ClipboardItem
from settings import special_paste
from settings.tool_settings import ToolSettingsManager
from ui.settings_ui.dialog import SettingsDialog
from ui.settings_ui.special_paste_dialog import SpecialPasteDialog


@pytest.fixture
def config(tmp_path, monkeypatch):
    manager = ToolSettingsManager(qsettings=QSettings(str(tmp_path / "settings.ini"), QSettings.Format.IniFormat))
    monkeypatch.setattr("settings.get_tool_settings_manager", lambda: manager)
    monkeypatch.setattr("ui.settings_ui.dialog.validate_global_hotkey_edits", lambda *_a, **_kw: True)
    return manager


def _controller(monkeypatch, kind="text", content="test"):
    from types import SimpleNamespace

    item = ClipboardItem(id=1, content=content, content_type=kind)
    monkeypatch.setattr(ClipboardController, "_load_settings", lambda self: None)
    return ClipboardController(SimpleNamespace(get_item=lambda _id: item, get_groups=lambda: []))


def _menu_keys(controller):
    submenu = next((action for action in controller.build_context_menu_data(1).actions
                    if action.key == "special_paste_menu"), None)
    if submenu is None:
        return []
    assert not submenu.children[0].is_separator
    assert not submenu.children[-1].is_separator
    assert not any(a.is_separator and b.is_separator
                   for a, b in zip(submenu.children, submenu.children[1:]))
    return [action.key for action in submenu.children if not action.is_separator]


def _only(*keys):
    return [(key, True) for key in keys] + [
        (key, False) for key, _enabled in special_paste.default_entries() if key not in keys
    ]


def test_saved_menu_is_read_again_without_recreating_the_clipboard_controller(config, monkeypatch):
    controller = _controller(monkeypatch)
    assert "transform_uppercase" in _menu_keys(controller)

    wanted = ["transform_extract_digits", "transform_remove_whitespace", "special_paste_plain_text"]
    special_paste.save(config, _only(*wanted))
    assert _menu_keys(controller) == wanted

    config.qsettings.sync()
    reopened = ToolSettingsManager(qsettings=QSettings(config.qsettings.fileName(), QSettings.Format.IniFormat))
    assert special_paste.load(reopened) == special_paste.load(config)


@pytest.mark.parametrize("kind", ["text", "file"])
def test_disabling_every_option_for_one_type_hides_only_its_submenu(config, monkeypatch, kind):
    entries = [(key, enabled and special_paste.OPTIONS_BY_KEY[key].content_type != kind)
               for key, enabled in special_paste.default_entries()]
    special_paste.save(config, entries)
    assert _menu_keys(_controller(monkeypatch, kind)) == []
    assert _menu_keys(_controller(monkeypatch, "file" if kind == "text" else "text"))


@pytest.mark.parametrize("payload", [
    "broken json", "null", "{}", '[null, 1, "ab", [[], true], ["unknown", false]]',
])
def test_unusable_saved_config_falls_back_to_a_working_default(config, monkeypatch, payload):
    config.set_app_setting(special_paste.SETTING_KEY, payload)
    assert special_paste.load(config) == special_paste.default_entries()
    assert "special_paste_plain_text" in _menu_keys(_controller(monkeypatch))


def test_partial_config_preserves_hidden_options_and_adds_new_presets(config):
    config.set_app_setting(special_paste.SETTING_KEY, json.dumps([
        ["transform_remove_whitespace", False], ["transform_remove_whitespace", True],
        ["file_paste_links", True],
    ]))
    entries = special_paste.load(config)
    assert entries[:2] == [("transform_remove_whitespace", False), ("file_paste_links", True)]
    assert dict(entries)["transform_extract_digits"] is False
    assert len(entries) == len(special_paste.BUILTIN_OPTIONS)


@pytest.mark.parametrize("key, expected", [
    ("transform_extract_digits", "0123"),
    ("transform_extract_letters", "AbcXYZ"),
])
def test_new_presets_paste_the_extracted_text(config, monkeypatch, key, expected):
    import pyclipboard

    special_paste.save(config, _only(key))
    controller = _controller(monkeypatch, content="编号 01-A_bc\n2.XYZ3 中文")
    written = []
    monkeypatch.setattr(pyclipboard, "set_clipboard_text", written.append)
    monkeypatch.setattr(controller, "_send_paste_keystroke", lambda explicit: None)
    assert _menu_keys(controller) == [key]
    assert controller.paste_transformed_text(1, key, explicit=True)
    assert written == [expected]


def test_editor_can_reorder_and_disable_options_then_restore_defaults(qapp):
    editor = SpecialPasteDialog(special_paste.default_entries())
    editor.show()
    qapp.processEvents()
    try:
        rows = editor._lists["text"].rows()
        target = next(row for row in rows if row.key == "transform_extract_letters")
        target.check.setChecked(True)
        rows[0].check.setChecked(False)
        top = rows[0].mapToGlobal(QPoint(0, 1)).y()
        editor._lists["text"]._drag_row(target, top)
        assert editor.entries()[0] == ("transform_extract_letters", True)
        assert dict(editor.entries())["special_paste_plain_text"] is False

        editor.tabs.setCurrentItem("file")
        assert editor._stack.currentWidget() is editor._lists["file"]
        editor._lists["file"].rows()[0].check.setChecked(False)
        editor.reset_button.click()
        assert editor.entries() == special_paste.default_entries()
    finally:
        editor.close()
        editor.deleteLater()


@pytest.mark.parametrize("height", [760, 1000])
def test_file_rows_stay_compact_when_the_window_has_extra_height(qapp, height):
    editor = SpecialPasteDialog(special_paste.default_entries())
    editor.resize(editor.width(), height)
    editor.show()
    qapp.processEvents()
    try:
        text_row_height = max(row.sizeHint().height() for row in editor._lists["text"].rows())
        editor.tabs.setCurrentItem("file")
        qapp.processEvents()
        file_list = editor._lists["file"]
        rows = file_list.rows()
        assert all(row.height() <= text_row_height + 4 for row in rows)
        assert rows[1].y() - rows[0].y() <= text_row_height + 4

        file_list._drag_row(rows[1], rows[0].mapToGlobal(QPoint(0, 1)).y())
        qapp.processEvents()
        assert [row.key for row in file_list.rows()] == ["file_paste_links", "file_paste_names"]
    finally:
        editor.close()
        editor.deleteLater()


def test_management_window_allows_screenshot_start_and_input(config, qapp):
    from types import SimpleNamespace

    from capture.quick_capture_controller import QuickCaptureController
    from core.qt_utils import blocking_modal
    from main_app import MainApp

    settings = SettingsDialog(config)
    settings._on_nav_changed(2, "clipboard")
    settings.show()
    screenshot = QPushButton("Screenshot window")
    screenshot.resize(160, 80)
    screenshot.show()
    clicked = []
    screenshot.clicked.connect(lambda: clicked.append(True))
    qapp.processEvents()
    queued = []
    app = SimpleNamespace(
        quick_capture=SimpleNamespace(busy=False, set_capture_pending=queued.append),
        screenshot_window=None, clipboard_window=None, _capture_pending=False,
        _capture_and_prepare_window=lambda: None,
    )
    app._activate_blocking_modal = lambda: MainApp._activate_blocking_modal(app)
    seen = {}

    def inspect():
        editor = QApplication.activeModalWidget()
        try:
            seen.update(
                modality=editor.windowModality(),
                settings_locked=blocking_modal(settings) is editor,
                quick_capture_blocked=QuickCaptureController._blocked(SimpleNamespace(main_app=app)),
            )
            MainApp.start_screenshot(app)
            QTest.mouseClick(screenshot.windowHandle(), Qt.MouseButton.LeftButton,
                             pos=screenshot.rect().center())
        finally:
            editor.reject()

    QTimer.singleShot(0, inspect)
    try:
        settings.special_paste_manage_btn.click()
        assert seen == {
            "modality": Qt.WindowModality.WindowModal,
            "settings_locked": True,
            "quick_capture_blocked": False,
        }
        assert app._capture_pending
        assert queued == [True]
        assert clicked == [True]
        assert not settings._has_unsaved_changes()
    finally:
        screenshot.close()
        screenshot.deleteLater()
        settings._skip_unsaved_close_prompt = True
        settings.close()
        settings.deleteLater()


@pytest.mark.parametrize("accept", [False, True])
def test_management_edits_stay_pending_until_settings_are_applied(config, monkeypatch, qapp, accept):
    dialog = SettingsDialog(config)
    dialog._on_nav_changed(2, "clipboard")
    original = special_paste.load(config)
    controller = _controller(monkeypatch)
    original_keys = _menu_keys(controller)
    edited = _only("transform_extract_digits", "transform_remove_whitespace")

    def edit(editor):
        editor._fill(edited)
        return QDialog.DialogCode.Accepted if accept else QDialog.DialogCode.Rejected

    monkeypatch.setattr(SpecialPasteDialog, "exec", edit)
    try:
        dialog.special_paste_manage_btn.click()
        assert special_paste.load(config) == original
        assert _menu_keys(controller) == original_keys
        assert dialog._has_unsaved_changes() == accept
        if accept:
            assert dialog._footer_ok_btn.isEnabled()
            assert dialog.apply_settings()
            assert _menu_keys(controller) == ["transform_extract_digits", "transform_remove_whitespace"]
            assert not dialog._has_unsaved_changes()
            dialog.refresh_settings()
            assert dialog.clipboard_special_paste_entries == edited
            dialog._reset_clipboard_page()
            assert dialog.clipboard_special_paste_entries == special_paste.default_entries()
            assert dialog._has_unsaved_changes()
    finally:
        dialog._skip_unsaved_close_prompt = True
        dialog.close()
        dialog.deleteLater()
