# -*- coding: utf-8 -*-
"""二级面板的颜色行：当前色命中哪个预设就圈哪个，都不命中时圈自定义色。"""
import pytest
from PySide6.QtGui import QColor

from ui.arrow_settings_panel import ArrowSettingsPanel
from ui.shape_settings_panel import ShapeSettingsPanel
from ui.text_settings_panel import TextSettingsPanel


def _picker(panel):
    return panel.color_btn if hasattr(panel, "color_btn") else panel.color_picker_btn


def _selected(panel):
    return [color_hex for swatch, color_hex in panel._preset_buttons if swatch.is_selected()]


@pytest.mark.parametrize("make_panel", [ShapeSettingsPanel, ArrowSettingsPanel, TextSettingsPanel])
def test_the_current_color_is_ringed_in_every_panel(qapp, make_panel):
    """面板上改颜色的入口最后都落到自定义色按钮的 set_color，所以在它上面验证就覆盖了全部入口"""
    panel = make_panel()
    picker = _picker(panel)
    try:
        picker.set_color(QColor("#0000FF"))
        assert _selected(panel) == ["#0000FF"]
        assert not picker._custom_active

        picker.set_color(QColor(255, 136, 0))
        assert _selected(panel) == []
        assert picker._custom_active

        # 透明度由面板单独调，不影响认出是哪个预设色
        translucent = QColor("#FF0000")
        translucent.setAlpha(128)
        picker.set_color(translucent)
        assert _selected(panel) == ["#FF0000"]
        assert not picker._custom_active
    finally:
        panel.deleteLater()


def test_clicking_a_preset_moves_the_ring(qapp):
    panel = ShapeSettingsPanel()
    try:
        swatches = {color_hex: swatch for swatch, color_hex in panel._preset_buttons}
        swatches["#00FF00"].click()
        assert _selected(panel) == ["#00FF00"]
        assert panel.current_color == QColor("#00FF00")
    finally:
        panel.deleteLater()
