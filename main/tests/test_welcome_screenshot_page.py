# -*- coding: utf-8 -*-
"""欢迎向导截图页：自动保存、保存格式、智能选区的回填与落盘"""
from PySide6.QtCore import QSettings

from settings.tool_settings import ToolSettingsManager
from ui.welcome.page2_screenshot import ScreenshotHotkeyPage


def _manager(tmp_path):
    settings = QSettings(
        str(tmp_path / "welcome_screenshot.ini"),
        QSettings.Format.IniFormat,
    )
    return ToolSettingsManager(qsettings=settings)


def test_settings_are_backfilled_from_config(qapp, tmp_path):
    manager = _manager(tmp_path)
    manager.set_screenshot_save_enabled(False)
    manager.set_screenshot_format("webp")
    manager.set_smart_selection(False)

    page = ScreenshotHotkeyPage(manager)
    try:
        assert page._autosave_toggle.isChecked() is False
        assert page._format_combo.currentData() == "WEBP"
        assert page._smart_toggle.isChecked() is False
    finally:
        page.close()


def test_save_writes_every_setting(qapp, tmp_path):
    manager = _manager(tmp_path)
    page = ScreenshotHotkeyPage(manager)
    try:
        page._autosave_toggle.setChecked(True)
        page._smart_toggle.setChecked(False)
        page._format_combo.setCurrentIndex(page._format_combo.findData("JPG"))
        page.save()

        assert manager.get_screenshot_save_enabled() is True
        assert manager.get_screenshot_format() == "JPG"
        assert manager.get_smart_selection() is False
    finally:
        page.close()


def test_save_location_follows_the_auto_save_toggle(qapp, tmp_path):
    """自动保存关掉后，位置和格式就不再生效，置灰免得误导用户。"""
    manager = _manager(tmp_path)
    page = ScreenshotHotkeyPage(manager)
    try:
        page._autosave_toggle.setChecked(True)
        assert page._path_edit.isEnabled()
        assert page._format_combo.isEnabled()

        page._autosave_toggle.setChecked(False)
        assert page._path_edit.isEnabled() is False
        assert page._browse_btn.isEnabled() is False
        assert page._format_combo.isEnabled() is False
        # 智能选区和保存无关，不该被一起置灰
        assert page._smart_toggle.isEnabled() is True
    finally:
        page.close()


def test_blank_path_keeps_the_configured_folder(qapp, tmp_path):
    manager = _manager(tmp_path)
    target = str(tmp_path / "shots")
    manager.set_screenshot_save_path(target)

    page = ScreenshotHotkeyPage(manager)
    try:
        page._path_edit.setText("")
        page.save()
        assert manager.get_screenshot_save_path() == target
    finally:
        page.close()
