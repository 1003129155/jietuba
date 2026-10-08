# -*- coding: utf-8 -*-
"""工具栏的二级面板和「…」弹层的宽度跟着内容走：右侧不留被隐藏控件或空格子的空位。"""
import pytest
from PySide6.QtCore import QPoint
from PySide6.QtWidgets import QWidget

from core.ui_scale import get_ui_scale, scaled
from settings import get_tool_settings_manager
from ui.toolbar import Toolbar
from ui.toolbar_layout import DEFAULT_ORDER, MORE, SETTING_KEY, SHOW, save_layout


@pytest.fixture(autouse=True)
def _clean_state():
    """排布写在会话共享的临时配置里，比例是进程级单例：用例前后都还原，期间解绑配置免得写进设置"""
    manager = get_tool_settings_manager()
    manager.set_app_setting(SETTING_KEY, "")
    scale = get_ui_scale()
    before, before_config = scale.percent, scale._config_manager
    scale._config_manager = None
    yield
    scale.set_percent(before)
    scale._config_manager = before_config
    manager.set_app_setting(SETTING_KEY, "")


def _right_gap(panel):
    """面板右缘到最右一个可见子控件右缘的距离"""
    right = 0
    for child in panel.findChildren(QWidget):
        if child.isVisible() and child.width() > 0:
            right = max(right, child.mapTo(panel, QPoint(0, 0)).x() + child.width())
    return panel.width() - right


# 画笔、荧光笔、聚光灯共用一个面板，各自隐藏不同的控件；来回切换才能暴露宽度没跟着收放
TOOL_SEQUENCE = ("pen", "highlighter", "spotlight", "pen", "spotlight", "highlighter",
                 "rect", "arrow", "number", "text", "mosaic", "pen")


@pytest.mark.parametrize("percent", [100, 150])
def test_panel_width_follows_its_visible_controls(qapp, percent):
    get_ui_scale().set_percent(percent)
    host = QWidget()    # 和截图窗口一样，面板是宿主的子部件
    host.resize(1600, 400)
    toolbar = Toolbar(host)
    host.show()
    try:
        for tool in TOOL_SEQUENCE:
            toolbar.select_tool(tool)
            shown = [panel for panel in toolbar._iter_panels() if panel.isVisible()]
            assert len(shown) == 1, tool
            margin = shown[0].layout().contentsMargins().right()
            assert abs(_right_gap(shown[0]) - margin) <= 1, tool
    finally:
        host.close()
        host.deleteLater()


def _fold(count):
    folded = ("pen", "highlighter", "mosaic", "arrow", "number", "rect")[:count]
    return [(key, MORE if key in folded else SHOW) for key in DEFAULT_ORDER]


@pytest.mark.parametrize("count", [1, 2, 3, 5, 6])
def test_more_popup_centers_a_grid_narrower_than_the_adjust_button(qapp, count):
    save_layout(_fold(count))
    toolbar = Toolbar()
    toolbar._show_more_popup()
    popup = toolbar._more_popup
    toolbar._hide_more_popup()
    popup.adjust_btn.setText("A rather long adjust label")    # 「调整」比网格宽
    toolbar._show_more_popup()

    pad = scaled(popup.BASE_PADDING)
    row = [toolbar._buttons[key] for key in toolbar._folded_keys][:popup.COLUMNS]
    left = min(button.x() for button in row) - pad
    right = popup.width() - pad - max(button.x() + button.width() for button in row)
    assert abs(left - right) <= 1
    adjust = popup.adjust_btn
    assert popup.width() - (adjust.x() + adjust.width()) == pad
