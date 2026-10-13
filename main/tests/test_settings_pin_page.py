"""钉图设置页：入口、从截图页和外观页搬来的设置、不透明度档位、跳到钉图快捷键。"""
import pytest
from PySide6.QtCore import QSettings
from PySide6.QtWidgets import QStackedWidget

from settings.tool_settings import ToolSettingsManager
from ui.settings_ui.dialog import SettingsDialog

PIN_PAGE = 11


@pytest.fixture
def manager(tmp_path, monkeypatch):
    manager = ToolSettingsManager(
        qsettings=QSettings(str(tmp_path / "settings.ini"), QSettings.Format.IniFormat)
    )
    manager.set_log_dir(str(tmp_path))
    monkeypatch.setattr("settings.get_tool_settings_manager", lambda: manager)
    monkeypatch.setattr("ui.settings_ui.dialog.log_info", lambda *_a, **_k: None)
    monkeypatch.setattr("core.shortcut_manager.HotkeySystem.check_hotkey_availability", lambda _s, _h: True)
    monkeypatch.setattr("ocr.get_available_engines", lambda: ["oneocr"])
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


def _open_pin_page(dialog):
    dialog._on_nav_changed(PIN_PAGE, "pin")
    return dialog._behavior_controls


def test_the_navigation_opens_the_pin_page(open_dialog):
    dialog = open_dialog()
    assert any(route == "pin" and index == PIN_PAGE for route, _i, _t, index, _p in dialog._nav_items)

    _open_pin_page(dialog)

    assert dialog.content_stack.currentIndex() == PIN_PAGE
    assert dialog.content_title.text() == "Pin Settings"


def test_pin_settings_moved_off_the_capture_and_appearance_pages(open_dialog):
    dialog = open_dialog()
    dialog._ensure_page(1)
    dialog._ensure_page(3)
    assert not hasattr(dialog, "pin_auto_toolbar_toggle")
    assert not hasattr(dialog, "ocr_enable_toggle")
    assert "pin_hover_buttons" not in dialog._behavior_controls

    controls = _open_pin_page(dialog)

    assert hasattr(dialog, "pin_auto_toolbar_toggle")
    assert hasattr(dialog, "ocr_enable_toggle")
    assert {"pin_auto_border", "pin_hover_buttons", "pin_default_opacity",
            "pin_click_through_opacity"} <= set(controls)


def test_opacity_combos_show_the_stored_values(open_dialog, manager):
    manager.set_app_setting("pin_default_opacity", 0.7)
    manager.set_app_setting("pin_click_through_opacity", 0.3)

    controls = _open_pin_page(open_dialog())

    assert controls["pin_default_opacity"].currentData() == 0.7
    assert controls["pin_click_through_opacity"].currentData() == 0.3


def test_an_off_step_stored_opacity_shows_the_default(open_dialog, manager):
    manager.set_app_setting("pin_click_through_opacity", 0.55)

    controls = _open_pin_page(open_dialog())

    assert controls["pin_click_through_opacity"].currentData() == 0.5


def test_applying_saves_the_pin_page(open_dialog, manager):
    dialog = open_dialog()
    controls = _open_pin_page(dialog)
    controls["pin_default_opacity"].setCurrentIndex(controls["pin_default_opacity"].findData(0.8))
    controls["pin_click_through_opacity"].setCurrentIndex(controls["pin_click_through_opacity"].findData(0.4))
    dialog.pin_auto_toolbar_toggle.setChecked(True)

    assert dialog.apply_settings()

    assert manager.get_app_setting("pin_default_opacity") == pytest.approx(0.8)
    assert manager.get_app_setting("pin_click_through_opacity") == pytest.approx(0.4)
    assert manager.get_pin_auto_toolbar() is True


def test_resetting_the_page_restores_the_defaults(open_dialog):
    dialog = open_dialog()
    controls = _open_pin_page(dialog)
    controls["pin_click_through_opacity"].setCurrentIndex(controls["pin_click_through_opacity"].findData(0.2))
    controls["pin_hover_buttons"].setChecked(True)
    dialog.pin_auto_toolbar_toggle.setChecked(True)

    dialog._reset_current_page()

    assert controls["pin_click_through_opacity"].currentData() == 0.5
    assert not controls["pin_hover_buttons"].isChecked()
    assert not dialog.pin_auto_toolbar_toggle.isChecked()


def test_ocr_toggle_is_off_and_disabled_without_an_engine(open_dialog, manager, monkeypatch):
    monkeypatch.setattr("ocr.get_available_engines", lambda: [])
    manager.set_ocr_enabled(True)
    dialog = open_dialog()

    _open_pin_page(dialog)

    assert dialog.ocr_enable_toggle.isEnabled() is False
    assert dialog.ocr_enable_toggle.isChecked() is False


@pytest.mark.parametrize("mouse, page, tabs_attr, stack_name", [
    (False, 0, "_inapp_shortcut_tabs", "InAppShortcutStack"),
    (True, 10, "_mouse_inapp_tabs", "MouseShortcutStack"),
])
def test_edit_buttons_open_the_pin_tab_of_the_shortcut_pages(open_dialog, mouse, page, tabs_attr, stack_name):
    dialog = open_dialog()
    _open_pin_page(dialog)

    dialog._show_pin_shortcuts(mouse=mouse)

    assert dialog.content_stack.currentIndex() == page
    assert getattr(dialog, tabs_attr).currentItem() == "pin"
    stack = dialog.content_stack.currentWidget().findChild(QStackedWidget, stack_name)
    # 两页的标签顺序不同：快捷键页是 截图/标注/钉图/剪贴板，鼠标页是 截图/钉图/剪贴板
    assert stack.currentIndex() == (1 if mouse else 2)
