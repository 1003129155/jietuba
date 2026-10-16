# -*- coding: utf-8 -*-
"""OCR 引擎设置：存取校验、设置页下拉框、保存后通知 OCR 模块。"""
import pytest
from PySide6.QtCore import QObject, QSettings, Signal
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


_DEFAULT_MODEL = {"id": "", "name": "PP-OCRv6 small", "det": "d", "rec": "r", "dict": None,
                  "catalog_id": "ppocr-v6-small"}
_KOREAN_MODEL = {"id": "ppocr-v5-korean", "name": "PP-OCRv5 Korean", "det": "d", "rec": "r", "dict": None,
                 "catalog_id": "ppocr-v5-korean"}


def _page(monkeypatch, tmp_path, available, engine, models=None, model_id=""):
    monkeypatch.setattr("ocr.get_available_engines", lambda: available)
    installed = [_DEFAULT_MODEL] if models is None else models
    monkeypatch.setattr("ocr.ppocr_models.installed", lambda: installed)
    manager = _manager(tmp_path)
    manager.set_ocr_engine(engine)
    manager.set_ppocr_model(model_id)
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


_MANAGE = ("manage_models", "Download more models...", True)


class _FakeModelDialog(QObject):
    """顶替模型管理窗口：设置窗口在这里是假对象，当不了真窗口的父控件。"""
    model_removed = Signal(str)

    def __init__(self, parent):
        super().__init__()
        self.status = ""

    def show_status(self, text):
        self.status = text


def test_list_has_only_choosable_entries_and_a_download_entry(monkeypatch, qapp, tmp_path):
    monkeypatch.setattr("ocr.get_ppocr_status", lambda: "available")
    dialog, page = _page(monkeypatch, tmp_path, ["oneocr", "ppocr_rust"], "ppocr_rust",
                         models=[_KOREAN_MODEL, _DEFAULT_MODEL], model_id="ppocr-v5-korean")
    try:
        items = [i for i in _items(dialog.ocr_engine_combo) if i[1]]  # 分隔线没有文字
        assert items == [
            ("auto", "Auto (Recommended)", True), ("oneocr", "Snipping Tool OCR", True),
            ("ppocr_rust:", "PP-OCRv6 small", True), ("ppocr_rust:ppocr-v5-korean", "PP-OCRv5 Korean", True),
            _MANAGE,
        ]
        assert dialog.ocr_engine_combo.currentData() == "ppocr_rust:ppocr-v5-korean"
    finally:
        page.deleteLater()
        qapp.processEvents()


def test_missing_snipping_tool_and_models_are_not_listed(monkeypatch, qapp, tmp_path):
    monkeypatch.setattr("ocr.get_ppocr_status", lambda: "missing_models")
    dialog, page = _page(monkeypatch, tmp_path, [], "auto", models=[])
    try:
        items = [i for i in _items(dialog.ocr_engine_combo) if i[1]]
        assert items == [("auto", "Auto (Recommended)", False), _MANAGE]
    finally:
        page.deleteLater()
        qapp.processEvents()


def test_build_without_ppocr_has_no_model_entries(monkeypatch, qapp, tmp_path):
    monkeypatch.setattr("ocr.get_ppocr_status", lambda: "missing_engine")
    dialog, page = _page(monkeypatch, tmp_path, ["oneocr"], "auto")
    try:
        assert _items(dialog.ocr_engine_combo) == [("auto", "Auto (Recommended)", True), ("oneocr", "Snipping Tool OCR", True)]
    finally:
        page.deleteLater()
        qapp.processEvents()


def test_no_engine_at_all_explains_how_to_get_one(monkeypatch, qapp, tmp_path):
    monkeypatch.setattr("ocr.get_ppocr_status", lambda: "missing_engine")
    dialog, page = _page(monkeypatch, tmp_path, [], "auto", models=[])
    try:
        assert not any(enabled for _, _, enabled in _items(dialog.ocr_engine_combo))
        assert any(text.startswith("OCR unavailable") for text in _texts(page))
    finally:
        page.deleteLater()
        qapp.processEvents()


def test_deleted_model_stays_selected(monkeypatch, qapp, tmp_path):
    """选中的模型被删了：照样列出并标明未安装，不悄悄换成别的模型。"""
    monkeypatch.setattr("ocr.get_ppocr_status", lambda: "missing_models")
    dialog, page = _page(monkeypatch, tmp_path, ["oneocr"], "ppocr_rust", models=[_DEFAULT_MODEL], model_id="mine")
    try:
        assert dialog.ocr_engine_combo.currentData() == "ppocr_rust:mine"
        assert ("ppocr_rust:mine", "mine (not installed)", False) in _items(dialog.ocr_engine_combo)
    finally:
        page.deleteLater()
        qapp.processEvents()


@pytest.mark.parametrize("saved_engine, removed, pending, setting_after, combo_after", [
    ("ppocr_rust", "ppocr-v5-korean", None, "auto", "auto"),                 # 删的正是在用的
    ("oneocr", "ppocr-v5-korean", None, "oneocr", "oneocr"),                 # 模型设置还在，但没在用
    ("ppocr_rust", "", None, "ppocr_rust", "ppocr_rust:ppocr-v5-korean"),     # 删的是别的模型
    ("oneocr", "ppocr-v5-korean", "ppocr_rust:ppocr-v5-korean", "oneocr", "auto"),  # 下拉框里选了、还没应用
])
def test_deleting_the_model_in_use_switches_to_auto(monkeypatch, qapp, tmp_path,
                                                     saved_engine, removed, pending, setting_after, combo_after):
    monkeypatch.setattr("ocr.get_ppocr_status", lambda: "available")
    dialog, page = _page(monkeypatch, tmp_path, ["oneocr", "ppocr_rust"], saved_engine,
                         models=[_DEFAULT_MODEL, _KOREAN_MODEL], model_id="ppocr-v5-korean")
    saved_choice = dialog.ocr_engine_selected
    dialog._settings_snapshot = {"ocr_engine_combo": saved_choice}
    switched, windows = [], []
    monkeypatch.setattr("ocr.set_ocr_engine", switched.append)

    def fake_exec(window, owner=None):
        windows.append(window)
        installed.remove(next(m for m in installed if m["id"] == removed))
        window.model_removed.emit(removed)
        return 0

    installed = [_DEFAULT_MODEL, _KOREAN_MODEL]
    monkeypatch.setattr("ocr.ppocr_models.installed", lambda: list(installed))
    monkeypatch.setattr("ui.dialogs.exec_dialog", fake_exec)
    monkeypatch.setattr("ui.settings_ui.ppocr_model_dialog.PpocrModelDialog", _FakeModelDialog)
    try:
        combo = dialog.ocr_engine_combo
        if pending:
            combo.setCurrentIndex(combo.findData(pending))
            combo.activated.emit(combo.currentIndex())
        combo.setCurrentIndex(combo.findData("manage_models"))
        combo.activated.emit(combo.currentIndex())
        in_use = setting_after == "auto"
        assert dialog.config_manager.get_ocr_engine() == setting_after
        assert switched == (["auto"] if in_use else [])
        assert combo.currentData() == combo_after
        assert bool(windows[0].status) is (combo_after == "auto")
        # 设置已经当场改了，快照跟着改，关窗时不会问要不要保存
        assert dialog._settings_snapshot["ocr_engine_combo"] == ("auto" if in_use else saved_choice)
    finally:
        page.deleteLater()
        qapp.processEvents()


def test_download_entry_opens_the_manager_and_keeps_the_choice(monkeypatch, qapp, tmp_path):
    monkeypatch.setattr("ocr.get_ppocr_status", lambda: "available")
    dialog, page = _page(monkeypatch, tmp_path, ["oneocr", "ppocr_rust"], "oneocr", models=[_DEFAULT_MODEL])
    opened = []

    def fake_exec(window, owner=None):
        opened.append(type(window).__name__)
        installed.append(_KOREAN_MODEL)   # 管理窗口里下载了韩文模型
        return 0

    installed = [_DEFAULT_MODEL]
    monkeypatch.setattr("ocr.ppocr_models.installed", lambda: list(installed))
    monkeypatch.setattr("ui.dialogs.exec_dialog", fake_exec)
    monkeypatch.setattr("ui.settings_ui.ppocr_model_dialog.PpocrModelDialog", _FakeModelDialog)
    try:
        combo = dialog.ocr_engine_combo
        combo.setCurrentIndex(combo.findData("manage_models"))
        combo.activated.emit(combo.currentIndex())
        assert opened == ["_FakeModelDialog"]
        assert combo.currentData() == "oneocr"
        assert combo.findData("ppocr_rust:ppocr-v5-korean") >= 0
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

        dialog.ocr_engine_combo.setCurrentIndex(dialog.ocr_engine_combo.findData("ppocr_rust:"))
        dialog.apply_settings()
        assert manager.get_ocr_engine() == "ppocr_rust"
        assert switched == ["ppocr_rust"]
        assert len(trims) == 1
    finally:
        dialog.deleteLater()
        qapp.processEvents()


def test_saving_a_new_model_switches_it_at_once(monkeypatch, qapp, tmp_path):
    manager = _manager(tmp_path)
    manager.set_log_dir(str(tmp_path))
    monkeypatch.setattr("ui.settings_ui.dialog.log_info", lambda *_args, **_kwargs: None)
    monkeypatch.setattr("core.shortcut_manager.HotkeySystem.check_hotkey_availability",
                        lambda _self, _hotkey: True)
    monkeypatch.setattr("ocr.get_ppocr_status", lambda: "available")
    monkeypatch.setattr("ocr.ppocr_models.installed", lambda: [_DEFAULT_MODEL, _KOREAN_MODEL])
    switched = []
    monkeypatch.setattr("ocr.set_ppocr_model", switched.append)
    dialog = SettingsDialog(manager)
    dialog.build_all_pages()
    for attr in ("log_toggle", "autostart_toggle", "language_combo", "_ui_theme_combo",
                 "_appearance_theme_color", "_appearance_mask_color", "_inapp_edits"):
        if hasattr(dialog, attr):
            delattr(dialog, attr)
    try:
        dialog.apply_settings()
        assert switched == []

        dialog.ocr_engine_combo.setCurrentIndex(dialog.ocr_engine_combo.findData("ppocr_rust:ppocr-v5-korean"))
        dialog.apply_settings()
        assert manager.get_ppocr_model() == "ppocr-v5-korean"
        assert manager.get_ocr_engine() == "ppocr_rust"
        assert switched == ["ppocr-v5-korean"]
    finally:
        dialog.deleteLater()
        qapp.processEvents()
