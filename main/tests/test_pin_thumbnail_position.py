# -*- coding: utf-8 -*-
"""缩略图进出后钉图回到原处；缩略图被拖动多少，还原后的钉图就跟着挪多少。"""
import pytest
from PySide6.QtCore import QPoint, QSettings, Qt
from PySide6.QtGui import QImage

from settings.tool_settings import ToolSettingsManager


@pytest.fixture
def config(tmp_path):
    manager = ToolSettingsManager(qsettings=QSettings(str(tmp_path / "settings.ini"), QSettings.IniFormat))
    manager.set_log_dir(str(tmp_path))
    manager.set_app_setting("ocr_enabled", False)
    manager.set_app_setting("pin_auto_toolbar", False)
    return manager


@pytest.fixture
def make_pin(qapp, config, monkeypatch):
    from pin.pin_ocr_manager import PinOCRManager
    from pin.pin_window import PinWindow
    monkeypatch.setattr(PinOCRManager, "init_now", lambda self: None)
    made = []

    def make(width, height, pos=QPoint(300, 200)):
        image = QImage(width, height, QImage.Format_ARGB32)
        image.fill(Qt.green)
        window = PinWindow(image, pos, config)
        qapp.processEvents()
        made.append(window)
        return window

    yield make
    for window in made:
        if not window._is_closed:
            window.close_window()


def _cursor_at(monkeypatch, point):
    monkeypatch.setattr("pin.pin_thumbnail.QCursor.pos", staticmethod(lambda: QPoint(point)))


@pytest.mark.parametrize("size", [(500, 350), (451, 301)])
@pytest.mark.parametrize("where", ["centre", "near_corner", "outside"])
def test_toggling_twice_returns_the_pin_to_where_it_was(make_pin, monkeypatch, qapp, size, where):
    pin = make_pin(*size)
    before = pin.geometry()
    point = {"centre": before.center(), "near_corner": before.topLeft() + QPoint(5, 5),
             "outside": before.bottomRight() + QPoint(200, 200)}[where]
    _cursor_at(monkeypatch, point)

    pin.toggle_thumbnail_mode()
    qapp.processEvents()
    assert pin._thumbnail_mode
    pin.toggle_thumbnail_mode()
    qapp.processEvents()

    assert pin.geometry() == before


def test_a_dragged_thumbnail_moves_the_restored_pin_by_the_same_amount(make_pin, monkeypatch, qapp):
    pin = make_pin(500, 350)
    before = pin.geometry()
    _cursor_at(monkeypatch, before.center())
    pin.toggle_thumbnail_mode()
    qapp.processEvents()
    pin.move(pin.pos() + QPoint(150, -40))
    pin.toggle_thumbnail_mode()
    qapp.processEvents()

    assert pin.geometry() == before.translated(150, -40)
