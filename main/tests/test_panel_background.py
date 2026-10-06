# -*- coding: utf-8 -*-
"""工具栏和二级面板共用的雾面底：缓存只在尺寸或比例变化时重画，主体不透明，四角透明。"""
import pytest
from PySide6.QtCore import Qt
from PySide6.QtGui import QImage
from PySide6.QtWidgets import QWidget

from core.ui_scale import get_ui_scale
from ui.base_settings_panel import paint_rounded_panel


class _Panel(QWidget):
    def __init__(self):
        super().__init__()
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)

    def paintEvent(self, event):
        paint_rounded_panel(self)


@pytest.fixture(autouse=True)
def restore_scale():
    """比例是进程级单例，改了要还原；期间解绑配置，免得写进设置"""
    manager = get_ui_scale()
    before, before_config = manager.percent, manager._config_manager
    manager._config_manager = None
    yield
    manager.set_percent(before)
    manager._config_manager = before_config


def _render(widget):
    image = QImage(widget.size(), QImage.Format.Format_ARGB32_Premultiplied)
    image.fill(0)
    widget.render(image)
    return image


def test_background_is_redrawn_only_when_size_or_scale_changes(qapp):
    panel = _Panel()
    panel.resize(300, 40)
    _render(panel)
    first = panel._panel_background[1]

    _render(panel)
    assert panel._panel_background[1] is first

    panel.resize(320, 40)
    _render(panel)
    resized = panel._panel_background[1]
    assert resized is not first

    get_ui_scale().set_percent(150)
    _render(panel)
    assert panel._panel_background[1] is not resized


def test_body_is_opaque_and_corners_stay_transparent(qapp):
    """主体不透明，叠在截图暗罩上才不会发灰"""
    panel = _Panel()
    panel.resize(300, 40)
    image = _render(panel)
    assert image.pixelColor(150, 20).alpha() == 255
    assert image.pixelColor(0, 0).alpha() == 0
