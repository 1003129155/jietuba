# -*- coding: utf-8 -*-
"""按住 Shift 拉直线的角度设置：存取校验、设置页下拉框、保存与恢复默认。"""
from types import SimpleNamespace

import pytest
from PySide6.QtCore import QSettings

from settings.tool_settings import SHIFT_LINE_ANGLES, ToolSettingsManager
from ui.settings_ui.dialog import SettingsDialog
from ui.settings_ui.page_quick_actions import create_quick_actions_page


def _manager(tmp_path):
    qsettings = QSettings(str(tmp_path / "shift_line.ini"), QSettings.Format.IniFormat)
    return ToolSettingsManager(qsettings=qsettings)


def test_default_snaps_every_45_degrees(tmp_path):
    assert _manager(tmp_path).get_shift_line_angle() == 45


@pytest.mark.parametrize("stored", ["", "abc", "60", "-45", "1.5"])
def test_unknown_stored_value_reads_as_the_default(tmp_path, stored):
    manager = _manager(tmp_path)
    manager.qsettings.setValue("app/shift_line_angle", stored)
    assert manager.get_shift_line_angle() == 45


def test_unknown_value_is_not_stored(tmp_path):
    manager = _manager(tmp_path)
    manager.set_shift_line_angle(15)
    manager.set_shift_line_angle(60)
    assert manager.get_shift_line_angle() == 45


@pytest.mark.parametrize("angle", SHIFT_LINE_ANGLES)
def test_every_angle_round_trips(tmp_path, angle):
    manager = _manager(tmp_path)
    manager.set_shift_line_angle(angle)
    assert manager.get_shift_line_angle() == angle


def _page(tmp_path, angle):
    manager = _manager(tmp_path)
    manager.set_shift_line_angle(angle)
    dialog = SimpleNamespace(config_manager=manager, tr=lambda text: text)
    return dialog, create_quick_actions_page(dialog)


def test_page_lists_every_angle_in_order_and_shows_the_stored_one(qapp, tmp_path):
    dialog, page = _page(tmp_path, 15)
    try:
        combo = dialog.shift_line_angle_combo
        assert [combo.itemData(i) for i in range(combo.count())] == list(SHIFT_LINE_ANGLES)
        assert combo.currentData() == 15
    finally:
        page.deleteLater()
        qapp.processEvents()


def test_reset_restores_the_default_angle(qapp, tmp_path):
    dialog, page = _page(tmp_path, 0)
    try:
        assert dialog.shift_line_angle_combo.currentData() == 0
        SettingsDialog._reset_quick_actions_page(dialog)
        assert dialog.shift_line_angle_combo.currentData() == 45
    finally:
        page.deleteLater()
        qapp.processEvents()


def test_saving_stores_the_chosen_angle(monkeypatch, qapp, tmp_path):
    manager = _manager(tmp_path)
    manager.set_log_dir(str(tmp_path))
    monkeypatch.setattr("ui.settings_ui.dialog.log_info", lambda *_args, **_kwargs: None)
    monkeypatch.setattr("core.shortcut_manager.HotkeySystem.check_hotkey_availability",
                        lambda _self, _hotkey: True)
    dialog = SettingsDialog(manager)
    dialog.build_all_pages()
    for attr in ("log_toggle", "autostart_toggle", "language_combo", "_ui_theme_combo",
                 "_appearance_theme_color", "_appearance_mask_color", "_inapp_edits"):
        if hasattr(dialog, attr):
            delattr(dialog, attr)
    try:
        assert dialog.shift_line_angle_combo.currentData() == 45
        dialog.shift_line_angle_combo.setCurrentIndex(SHIFT_LINE_ANGLES.index(0))
        dialog.apply_settings()
        assert manager.get_shift_line_angle() == 0
    finally:
        dialog.deleteLater()
        qapp.processEvents()
