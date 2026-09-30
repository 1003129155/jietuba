"""实时局部采样与普通截图全屏图像使用相同的放大镜绘制。"""

from types import SimpleNamespace

import pytest
from PySide6.QtCore import QPointF, QRect, QRectF
from PySide6.QtGui import QColor, QImage
from PySide6.QtWidgets import QWidget

from settings import get_tool_settings_manager
from ui.magnifier import MagnifierOverlay


@pytest.fixture
def session(qapp):
    parent = QWidget()
    parent.setGeometry(-200, -100, 500, 400)
    image = QImage(500, 400, QImage.Format.Format_RGB32)
    image.fill(QColor("#2299DD"))
    scene = SimpleNamespace(
        scene_rect=QRectF(-200, -100, 500, 400),
        background=SimpleNamespace(image=lambda: image),
        selection_model=SimpleNamespace(is_confirmed=False),
        tool_controller=SimpleNamespace(current_tool_id="cursor"),
    )
    view = SimpleNamespace(drawing=SimpleNamespace(active=False), text_drag=SimpleNamespace(active=False))
    magnifier = MagnifierOverlay(parent, scene, view, get_tool_settings_manager())
    yield magnifier, parent, scene, view
    parent.close()
    parent.deleteLater()


def test_live_patch_matches_full_desktop_pixels_and_coordinates(session):
    magnifier, parent, scene, view = session
    magnifier.update_cursor(QPointF(-40, 20))
    original = magnifier.grab().toImage()
    original_color = magnifier.get_color_info_text()
    rect = QRect(-72, -12, 64, 64)
    patch = scene.background.image().copy(rect.translated(200, 100))
    magnifier.set_sample_image(patch, rect)
    assert magnifier.get_color_info_text() == original_color
    assert magnifier._scene_to_image_point(QPointF(-40, 20)).x() == 32
    assert magnifier.grab().toImage() == original
    assert scene.scene_rect == QRectF(-200, -100, 500, 400), "字号仍按全屏范围计算"


def test_stationary_cursor_refreshes_pixels_and_color_together(session):
    magnifier, *_ = session
    magnifier.update_cursor(QPointF(-40, 20))
    rect = QRect(-72, -12, 64, 64)
    sample = QImage(64, 64, QImage.Format.Format_RGB32)
    sample.fill(QColor("#FF2244"))
    magnifier.set_sample_image(sample, rect)
    first = magnifier.grab().toImage()
    first_color = magnifier.get_color_info_text()
    sample.fill(QColor("#118844"))
    magnifier.set_sample_image(sample, rect)
    assert magnifier.grab().toImage() != first
    assert magnifier.get_color_info_text() != first_color
    assert magnifier._cached_sample_image.pixelColor(0, 0) == QColor("#118844")


def test_sample_owns_a_stable_image_and_rebind_restores_original_source(session):
    magnifier, parent, scene, view = session
    magnifier.update_cursor(QPointF(-40, 20))
    sample = QImage(64, 64, QImage.Format.Format_RGB32)
    sample.fill(QColor("#FF2244"))
    magnifier.set_sample_image(sample, QRect(-72, -12, 64, 64))
    sample.fill(QColor("#118844"))
    assert magnifier._background_image().pixelColor(0, 0) == QColor("#FF2244")
    magnifier.rebind(scene, view)
    assert magnifier._sample_image is None
    assert magnifier._sample_rect is None
    assert magnifier._background_image() is scene.background.image()
