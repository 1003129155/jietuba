# -*- coding: utf-8 -*-
"""截图引擎设置：存取、开发者页下拉框、启动预热按引擎分流。"""
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import MagicMock, patch

import pytest
from PySide6.QtCore import QSettings, QTimer, QTranslator

from settings.tool_settings import CAPTURE_ENGINES, ToolSettingsManager
from ui.settings_ui.page_developer import create_developer_page


def _manager(tmp_path):
    qsettings = QSettings(str(tmp_path / "capture_engine.ini"), QSettings.Format.IniFormat)
    return ToolSettingsManager(qsettings=qsettings)


def _dialog(manager):
    return SimpleNamespace(config_manager=manager, tr=lambda text: text,
                           _open_welcome_wizard=lambda: None)


def test_capture_engine_defaults_to_auto(tmp_path):
    assert _manager(tmp_path).get_capture_engine() == "auto"


@pytest.mark.parametrize("engine", CAPTURE_ENGINES)
def test_capture_engine_round_trips(tmp_path, engine):
    manager = _manager(tmp_path)
    manager.set_capture_engine(engine)
    assert manager.get_capture_engine() == engine


def test_unknown_capture_engine_reads_as_auto(tmp_path):
    manager = _manager(tmp_path)
    manager.qsettings.setValue("app/capture_engine", "gdi")
    assert manager.get_capture_engine() == "auto"
    manager.set_capture_engine("gdi")
    assert manager.qsettings.value("app/capture_engine") == "auto"


def test_reset_restores_auto(tmp_path):
    manager = _manager(tmp_path)
    manager.set_capture_engine("mss")
    manager.reset_app_settings()
    assert manager.get_capture_engine() == "auto"


@pytest.mark.parametrize("engine", CAPTURE_ENGINES)
def test_developer_page_loads_capture_engine(qapp, tmp_path, engine):
    manager = _manager(tmp_path)
    manager.set_capture_engine(engine)
    dialog = _dialog(manager)
    page = create_developer_page(dialog)
    try:
        combo = dialog.capture_engine_combo
        assert combo.currentData() == engine
        assert [combo.itemData(i) for i in range(combo.count())] == list(CAPTURE_ENGINES)
    finally:
        page.deleteLater()


@pytest.mark.parametrize("language", ["en", "ja", "ko", "zh"])
def test_capture_engine_card_texts_are_translated_in_every_language(qapp, tmp_path, language):
    """文案取自页面上真正显示的控件；MSS / HDR 是引擎名，不走翻译。"""
    translations = Path(__file__).parents[1] / "translations"
    translator = QTranslator()
    assert translator.load(str(translations / f"app_{language}.qm"))

    dialog = _dialog(_manager(tmp_path))
    page = create_developer_page(dialog)
    try:
        combo = dialog.capture_engine_combo
        card = combo.parentWidget()
        while not hasattr(card, "titleLabel"):
            card = card.parentWidget()
        sources = [card.titleLabel.text(), card.contentLabel.text(), combo.itemText(0)]
        assert all(sources)
        for source in sources:
            assert translator.translate("SettingsDialog", source), (language, source)
    finally:
        page.deleteLater()


class _PreloadConfig:
    """只开截图预加载，其余预加载步骤都关掉。"""

    def __init__(self, engine):
        self.engine = engine

    def get_app_setting(self, key, default=None):
        return key == "preload_screenshot"

    def get_capture_engine(self):
        return self.engine


@pytest.mark.parametrize("engine, warm_up_mss, warm_up_hdr", [
    ("auto", True, True),
    ("mss", True, False),
    ("hdr", False, True),
])
def test_preload_warms_up_only_the_engines_in_use(engine, warm_up_mss, warm_up_hdr):
    from core.bootstrap import PreloadManager

    manager = PreloadManager(SimpleNamespace(config_manager=_PreloadConfig(engine)))
    manager._preload_screenshot_modules = MagicMock(return_value=True)
    with patch.object(QTimer, "singleShot"):
        manager.build_and_start()

    manager._steps[0]()
    manager._preload_screenshot_modules.assert_called_once_with(warm_up_mss=warm_up_mss)
    assert (manager._preload_hdr_session in manager._steps) is warm_up_hdr
