"""序号画出来的大小必须等于面板显示的大小。

工具栏的 set_stroke_width 会把同一个笔触宽度广播给所有设置面板，但各面板的
合法范围不同：画笔可以细到 1，序号最小是 8。以前面板会把显示钳到 8，绘制却
仍然用原始的小值，于是出现"面板写着 8、画出来只有 1 那么大"。
"""

import pytest

from tools.number import NumberTool
from ui.number_settings_panel import NumberSettingsPanel


def test_panel_range_comes_from_the_tool(qapp):
    """两处各写一份数字迟早会漂移，范围只能有一个出处。"""
    assert NumberSettingsPanel.SIZE_RANGE == (NumberTool.MIN_WIDTH, NumberTool.MAX_WIDTH)


@pytest.mark.parametrize("broadcast_width", [1, 2, 3, 5, 7])
def test_a_width_below_the_minimum_still_draws_at_the_minimum(qapp, broadcast_width):
    panel = NumberSettingsPanel()
    panel.set_size(broadcast_width)

    shown = panel.size_spin.value()
    assert shown == NumberTool.MIN_WIDTH

    radius = NumberTool.get_radius_for_width(broadcast_width)
    assert radius == NumberTool.MIN_WIDTH * NumberTool.RADIUS_SCALE


@pytest.mark.parametrize("width", [1, 3, 8, 11, 16, 72, 200])
def test_drawn_radius_always_matches_what_the_panel_shows(qapp, width):
    panel = NumberSettingsPanel()
    panel.set_size(width)

    shown = panel.size_spin.value()
    radius = NumberTool.get_radius_for_width(width)
    assert radius == shown * NumberTool.RADIUS_SCALE


@pytest.mark.parametrize("width", [1, 3, 8, 11, 90])
def test_panel_state_agrees_with_the_stepper(qapp, width):
    """current_size 以前存原始值，和步进器显示的钳制值对不上。"""
    panel = NumberSettingsPanel()
    panel.set_size(width)

    assert panel.current_size == panel.size_spin.value()


def test_clamp_survives_rubbish_input(qapp):
    assert NumberTool.clamp_width(None) == NumberTool.MIN_WIDTH
    assert NumberTool.clamp_width("nonsense") == NumberTool.MIN_WIDTH
    assert NumberTool.clamp_width(-40) == NumberTool.MIN_WIDTH
    assert NumberTool.clamp_width(10 ** 9) == NumberTool.MAX_WIDTH
