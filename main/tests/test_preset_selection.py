"""直接给出的选区（全局鼠标「截图并编辑」、恢复上次选区）和拖选确认一样出工具栏，区域原样保留。"""

import pytest
from PySide6.QtCore import QRect, QRectF, Qt
from PySide6.QtGui import QImage

import core.last_capture_region as region_module
from core.last_capture_region import set_last_region
from pin.pin_manager import PinManager
from settings.tool_settings import ToolSettingsManager
from ui.screenshot_window import ScreenshotWindow


@pytest.fixture
def window(qapp, tmp_settings, monkeypatch):
    monkeypatch.setattr(PinManager, "suppress_topmost", lambda self: None)
    monkeypatch.setattr(PinManager, "restore_topmost", lambda self: None)
    monkeypatch.setattr(region_module, "_last_region", None)
    image = QImage(400, 300, QImage.Format.Format_ARGB32)
    image.fill(Qt.GlobalColor.white)
    shot = ScreenshotWindow(ToolSettingsManager(tmp_settings), prefetched_image=image,
                            prefetched_rect=QRectF(0, 0, 400, 300))
    yield shot
    shot.cleanup_and_close()
    shot.deleteLater()


def test_preset_selection_shows_the_toolbar_and_keeps_the_exact_region(window):
    window.scene.preset_selection(QRectF(10, 20, 3, 4))
    model = window.scene.selection_model
    assert model.is_confirmed
    assert model.rect() == QRectF(10, 20, 3, 4)
    assert window.toolbar.isVisible()


def test_restoring_the_last_region_shows_the_toolbar(window):
    set_last_region(QRect(30, 40, 120, 90))
    assert window._shortcut_handler._restore_last_region()
    assert window.scene.selection_model.rect() == QRectF(30, 40, 120, 90)
    assert window.toolbar.isVisible()
