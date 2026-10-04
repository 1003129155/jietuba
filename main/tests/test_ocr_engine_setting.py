# -*- coding: utf-8 -*-
"""OCR 引擎设置：存取校验、设置页下拉框、保存后通知 OCR 模块。"""
import pytest
from PySide6.QtCore import QSettings
from types import SimpleNamespace

from settings.tool_settings import OCR_ENGINES, ToolSettingsManager
from ui.settings_ui.dialog import SettingsDialog
from ui.settings_ui.page_capture import create_capture_page


def _manager(tmp_path):
    qsettings = QSettings(str(tmp_path / "ocr_settings.ini"), QSettings.Format.IniFormat)
    return ToolSettingsManager(qsettings=qsettings)


def test_default_is_auto(tmp_path):
    assert _manager(tmp_path).get_ocr_engine() == "auto"


@pytest.mark.parametrize("stored", ["windos_ocr", "windows_media_ocr", "", "nonsense"])
def test_unknown_stored_value_reads_as_auto(tmp_path, stored):
    manager = _manager(tmp_path)
    manager.qsettings.setValue("app/ocr_engine", stored)
    assert manager.get_ocr_engine() == "auto"


def test_unknown_value_is_not_stored(tmp_path):
    manager = _manager(tmp_path)
    manager.set_ocr_engine("ppocr_rust")
    manager.set_ocr_engine("windows_media_ocr")
    assert manager.get_ocr_engine() == "auto"


def _page(monkeypatch, tmp_path, available, engine):
    monkeypatch.setattr("ocr.get_available_engines", lambda: available)
    manager = _manager(tmp_path)
    manager.set_ocr_engine(engine)
    dialog = SimpleNamespace(
        config_manager=manager, tr=lambda text: text,
        _change_save_dir=lambda: None, _open_save_dir=lambda: None,
    )
    return dialog, create_capture_page(dialog)


def _items(combo):
    return [(combo.itemData(i), combo.itemText(i), combo.model().item(i).isEnabled())
            for i in range(combo.count())]


def _texts(page):
    from PySide6.QtWidgets import QLabel
    return [label.text() for label in page.findChildren(QLabel)]


def test_full_build_on_windows_11_offers_every_engine(monkeypatch, qapp, tmp_path):
    dialog, page = _page(monkeypatch, tmp_path, ["oneocr", "ppocr_rust"], "ppocr_rust")
    try:
        assert [data for data, _, enabled in _items(dialog.ocr_engine_combo) if enabled] == list(OCR_ENGINES)
        assert dialog.ocr_engine_combo.currentData() == "ppocr_rust"
    finally:
        page.deleteLater()
        qapp.processEvents()


@pytest.mark.parametrize("status, label", [
    ("missing_engine", "PP-OCR (missing engine)"),
    ("missing_models", "PP-OCR (missing models)"),
])
def test_unavailable_ppocr_explains_missing_files(monkeypatch, qapp, tmp_path, status, label):
    monkeypatch.setattr("ocr.get_ppocr_status", lambda: status)
    dialog, page = _page(monkeypatch, tmp_path, ["oneocr"], "auto")
    try:
        items = _items(dialog.ocr_engine_combo)
        assert items[2] == ("ppocr_rust", label, False)
        assert items[1][2] is True
    finally:
        page.deleteLater()
        qapp.processEvents()


def test_no_engine_at_all_explains_how_to_get_one(monkeypatch, qapp, tmp_path):
    dialog, page = _page(monkeypatch, tmp_path, [], "auto")
    try:
        assert not any(enabled for _, _, enabled in _items(dialog.ocr_engine_combo))
        assert dialog.ocr_enable_toggle.isEnabled() is False
        assert any(text.startswith("OCR unavailable") for text in _texts(page))
    finally:
        page.deleteLater()
        qapp.processEvents()


def test_saving_a_new_engine_switches_it_at_once(monkeypatch, qapp, tmp_path):
    manager = _manager(tmp_path)
    manager.set_log_dir(str(tmp_path))
    monkeypatch.setattr("ui.settings_ui.dialog.log_info", lambda *_args, **_kwargs: None)
    monkeypatch.setattr("core.shortcut_manager.HotkeySystem.check_hotkey_availability",
                        lambda _self, _hotkey: True)
    switched, trims = [], []
    monkeypatch.setattr("ocr.set_ocr_engine", switched.append)
    monkeypatch.setattr("core.platform_utils.request_trim_working_set", lambda *a: trims.append(a))
    dialog = SettingsDialog(manager)
    dialog.build_all_pages()
    for attr in ("log_toggle", "autostart_toggle", "language_combo", "_ui_theme_combo",
                 "_appearance_theme_color", "_appearance_mask_color", "_inapp_edits"):
        if hasattr(dialog, attr):
            delattr(dialog, attr)
    try:
        dialog.apply_settings()
        assert switched == []          # 没改引擎就不动已加载的引擎

        dialog.ocr_engine_combo.setCurrentIndex(OCR_ENGINES.index("ppocr_rust"))
        dialog.apply_settings()
        assert manager.get_ocr_engine() == "ppocr_rust"
        assert switched == ["ppocr_rust"]
        assert len(trims) == 1
    finally:
        dialog.deleteLater()
        qapp.processEvents()
