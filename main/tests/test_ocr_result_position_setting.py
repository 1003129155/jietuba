# -*- coding: utf-8 -*-
"""OCR 结果窗口位置设置：存取校验、设置页下拉框、保存与恢复默认。"""
from types import SimpleNamespace

import pytest
from PySide6.QtCore import QSettings

from settings.tool_settings import OCR_RESULT_POSITIONS, ToolSettingsManager
from ui.settings_ui.dialog import SettingsDialog
from ui.settings_ui.page_capture import create_capture_page


def _manager(tmp_path):
    qsettings = QSettings(str(tmp_path / "ocr_position.ini"), QSettings.Format.IniFormat)
    return ToolSettingsManager(qsettings=qsettings)


def test_default_is_the_display_with_the_capture_area(tmp_path):
    assert _manager(tmp_path).get_ocr_result_position() == "region_screen"


@pytest.mark.parametrize("stored", ["", "center", "beside", "nonsense"])
def test_unknown_stored_value_reads_as_the_default(tmp_path, stored):
    manager = _manager(tmp_path)
    manager.qsettings.setValue("app/ocr_result_position", stored)
    assert manager.get_ocr_result_position() == "region_screen"


def test_stored_value_is_matched_ignoring_case(tmp_path):
    manager = _manager(tmp_path)
    manager.qsettings.setValue("app/ocr_result_position", "Beside_Region")
    assert manager.get_ocr_result_position() == "beside_region"


def test_unknown_value_is_not_stored(tmp_path):
    manager = _manager(tmp_path)
    manager.set_ocr_result_position("beside_region")
    manager.set_ocr_result_position("nonsense")
    assert manager.get_ocr_result_position() == "region_screen"


@pytest.mark.parametrize("position", OCR_RESULT_POSITIONS)
def test_every_position_round_trips(tmp_path, position):
    manager = _manager(tmp_path)
    manager.set_ocr_result_position(position)
    assert manager.get_ocr_result_position() == position


def test_last_center_has_no_default(tmp_path):
    assert _manager(tmp_path).get_ocr_result_last_center() is None


@pytest.mark.parametrize("center", [(900, 500), (-1200, 300), (0, 0)])
def test_last_center_round_trips(tmp_path, center):
    manager = _manager(tmp_path)
    manager.set_ocr_result_last_center(*center)
    assert manager.get_ocr_result_last_center() == center


@pytest.mark.parametrize("stored", ["", "abc", "1", "1,2,3", "x,y", "1.5,2"])
def test_a_broken_last_center_reads_as_none(tmp_path, stored):
    manager = _manager(tmp_path)
    manager.qsettings.setValue("app/ocr_result_last_center", stored)
    assert manager.get_ocr_result_last_center() is None


def _page(monkeypatch, tmp_path, position):
    monkeypatch.setattr("ocr.get_available_engines", lambda: ["oneocr"])
    manager = _manager(tmp_path)
    manager.set_ocr_result_position(position)
    dialog = SimpleNamespace(
        config_manager=manager, tr=lambda text: text,
        _change_save_dir=lambda: None, _open_save_dir=lambda: None,
    )
    return dialog, create_capture_page(dialog)


def test_page_lists_every_position_in_order_and_shows_the_stored_one(monkeypatch, qapp, tmp_path):
    dialog, page = _page(monkeypatch, tmp_path, "beside_region")
    try:
        combo = dialog.ocr_result_position_combo
        assert [combo.itemData(i) for i in range(combo.count())] == list(OCR_RESULT_POSITIONS)
        assert combo.currentData() == "beside_region"
    finally:
        page.deleteLater()
        qapp.processEvents()


def test_reset_restores_the_default_position(monkeypatch, qapp, tmp_path):
    dialog, page = _page(monkeypatch, tmp_path, "primary_screen")
    try:
        assert dialog.ocr_result_position_combo.currentData() == "primary_screen"
        SettingsDialog._reset_screenshot_settings_page(dialog)
        assert dialog.ocr_result_position_combo.currentData() == "region_screen"
    finally:
        page.deleteLater()
        qapp.processEvents()


def test_saving_stores_the_chosen_position(monkeypatch, qapp, tmp_path):
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
        assert dialog.ocr_result_position_combo.currentData() == "region_screen"
        dialog.ocr_result_position_combo.setCurrentIndex(OCR_RESULT_POSITIONS.index("beside_region"))
        dialog.apply_settings()
        assert manager.get_ocr_result_position() == "beside_region"
    finally:
        dialog.deleteLater()
        qapp.processEvents()
