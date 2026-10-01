"""设置窗口按需建页：打开时只建快捷键页，其余页第一次切到时才建。"""
import pytest
from PySide6.QtCore import QSettings

from settings.settings_transfer import export_settings, read_settings_file
from settings.tool_settings import ToolSettingsManager
from ui.settings_ui.dialog import _PAGE_BUILDERS, SettingsDialog


def _manager(path):
    return ToolSettingsManager(qsettings=QSettings(str(path), QSettings.Format.IniFormat))


@pytest.fixture
def manager(tmp_path, monkeypatch):
    manager = _manager(tmp_path / "settings.ini")
    manager.set_log_dir(str(tmp_path))
    # 外观页有几项读全局配置；程序里它和窗口用的是同一份
    monkeypatch.setattr("settings.get_tool_settings_manager", lambda: manager)
    monkeypatch.setattr("ui.settings_ui.dialog.log_info", lambda *_a, **_k: None)
    monkeypatch.setattr("core.shortcut_manager.HotkeySystem.check_hotkey_availability", lambda _s, _h: True)
    return manager


@pytest.fixture
def open_dialog(qapp, manager):
    dialogs = []

    def make():
        dialog = SettingsDialog(manager, manager.get_hotkey())
        dialogs.append(dialog)
        return dialog

    yield make
    for dialog in dialogs:
        dialog._skip_unsaved_close_prompt = True
        dialog.close()
        dialog.deleteLater()
    qapp.processEvents()


def test_opening_builds_only_the_shortcut_page(open_dialog):
    dialog = open_dialog()

    assert dialog._built_pages == {0}
    assert dialog.content_stack.count() == len(_PAGE_BUILDERS)
    assert hasattr(dialog, "hotkey_input")
    assert not hasattr(dialog, "capture_engine_combo")
    assert not hasattr(dialog, "provider_field_widgets")


def test_a_page_is_built_on_first_visit_and_reused(open_dialog):
    dialog = open_dialog()

    dialog._on_nav_changed(1, "capture")
    page = dialog.content_stack.widget(1)
    assert hasattr(dialog, "capture_engine_combo")
    assert dialog.content_stack.currentWidget() is page

    dialog._on_nav_changed(0, "shortcuts")
    dialog._on_nav_changed(1, "capture")
    assert dialog.content_stack.widget(1) is page
    assert dialog.content_stack.count() == len(_PAGE_BUILDERS)


@pytest.mark.parametrize("index", range(len(_PAGE_BUILDERS)))
def test_any_page_can_be_built_first_without_counting_as_a_change(open_dialog, index):
    dialog = open_dialog()

    dialog._ensure_page(index)

    assert index in dialog._built_pages
    assert not dialog._has_unsaved_changes()


def test_pages_built_later_show_what_a_refresh_from_config_shows(open_dialog, manager):
    manager.set_capture_engine("mss")
    manager.set_clipboard_history_limit(500)
    manager.set_scroll_cooldown(0.3)
    manager.set_ocr_copy_directly_enabled(True)
    manager.set_app_setting("pin_hover_buttons", True)
    manager.set_app_setting("mouse_capture_close", "middle")
    dialog = open_dialog()

    dialog.build_all_pages()
    built = dialog._snapshot_settings()
    dialog.refresh_settings()

    assert dialog._snapshot_settings() == built


def test_edits_survive_building_another_page(open_dialog):
    dialog = open_dialog()
    original = dialog.hotkey_input.text()

    dialog.hotkey_input.setText("ctrl+alt+9")
    dialog._on_nav_changed(4, "translation")

    assert dialog.hotkey_input.text() == "ctrl+alt+9"
    assert dialog._has_unsaved_changes()
    dialog.hotkey_input.setText(original)
    assert not dialog._has_unsaved_changes()


def test_export_includes_pages_never_opened(open_dialog):
    keys = open_dialog().settings_keys()

    reference = open_dialog()
    reference.build_all_pages()
    assert keys == reference.settings_keys()
    assert {"app/capture_engine", "pin/hover_buttons", "screenshot/scroll_cooldown"} <= keys


def test_import_fills_pages_never_opened(open_dialog, tmp_path):
    source = _manager(tmp_path / "source.ini")
    source.set_capture_engine("mss")
    source.set_clipboard_history_limit(500)
    file = tmp_path / "import.json"
    export_settings(source, source.qsettings.allKeys(), str(file))
    dialog = open_dialog()

    dialog.load_imported_settings(read_settings_file(str(file)))

    assert dialog.capture_engine_combo.currentData() == "mss"
    assert dialog.clipboard_history_limit_spin.value() == 500
    assert dialog._has_unsaved_changes()


def test_applying_with_only_the_first_page_leaves_other_settings_alone(open_dialog, manager):
    manager.set_capture_engine("mss")
    dialog = open_dialog()

    dialog.hotkey_input.setText("ctrl+alt+9")
    assert dialog.apply_settings()

    assert manager.get_hotkey() == "ctrl+alt+9"
    assert manager.get_capture_engine() == "mss"


def test_showing_again_does_not_restyle_the_window(qapp, open_dialog, monkeypatch):
    applied = []
    original = SettingsDialog.setStyleSheet
    monkeypatch.setattr(SettingsDialog, "setStyleSheet",
                        lambda self, sheet: (applied.append(sheet), original(self, sheet))[1])
    dialog = open_dialog()
    dialog.show()
    qapp.processEvents()
    applied.clear()

    dialog.hide()
    dialog.show()
    qapp.processEvents()
    assert applied == []

    monkeypatch.setattr("ui.settings_ui.dialog.theme_surface_color", lambda: "#123456")
    dialog._on_ui_theme_changed(None)
    assert len(applied) == 1
    assert "#123456" in dialog.styleSheet()
