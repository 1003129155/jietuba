"""笔触宽度只在一个地方被钳制：写进 ToolContext 的时候。

工具栏的 set_stroke_width 会把同一个宽度广播给所有工具和面板，但各工具能接受
的范围不同——画笔可以细到 1，序号圈小于 8 就看不清里面的数字。以前面板把显示
钳到 8、绘制却仍按原始的小值算半径，于是"面板写着 8、画出来只有 1 那么大"。

现在范围由工具声明，钳制发生在写入 ctx 的入口，下游（绘制、光标、面板）直接
取用 ctx 的值即可。这里锁住这个不变量。
"""

import pytest
from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import QColor, QImage

from canvas.items import NumberItem
from canvas.scene import CanvasScene
from tools.base import Tool
from tools.number import NumberTool
from tools.pen import PenTool
from ui.number_settings_panel import NumberSettingsPanel


@pytest.fixture
def scene(qapp):
    image = QImage(300, 200, QImage.Format.Format_ARGB32_Premultiplied)
    image.fill(QColor("white"))
    canvas = CanvasScene(image, QRectF(0, 0, 300, 200))
    canvas.selection_model.initialize_confirmed_rect(QRectF(0, 0, 300, 200))
    yield canvas


def test_the_range_is_declared_once_per_tool(qapp):
    """范围只有一份出处：工具自己。面板和绘制都从它派生。"""
    assert (NumberTool.MIN_WIDTH, NumberTool.MAX_WIDTH) != (Tool.MIN_WIDTH, Tool.MAX_WIDTH)
    assert NumberSettingsPanel.SIZE_RANGE == (NumberTool.MIN_WIDTH, NumberTool.MAX_WIDTH)
    # 画笔沿用基类的宽范围，不该被序号的收窄影响
    assert (PenTool.MIN_WIDTH, PenTool.MAX_WIDTH) == (Tool.MIN_WIDTH, Tool.MAX_WIDTH)


@pytest.mark.parametrize(
    ("broadcast", "expected"),
    [(1, 8), (3, 8), (7, 8), (8, 8), (20, 20), (72, 72), (200, 72)],
)
def test_ctx_only_ever_holds_a_width_the_active_tool_accepts(scene, broadcast, expected):
    scene.activate_tool("number")
    scene.update_style(width=broadcast)

    assert scene.tool_controller.ctx.stroke_width == expected


@pytest.mark.parametrize("broadcast", [1, 3, 8, 20, 200])
def test_drawn_radius_is_a_plain_multiple_of_the_ctx_width(scene, broadcast):
    """下游不再各自钳制，半径就是 ctx 值乘以固定比例。"""
    scene.activate_tool("number")
    scene.update_style(width=broadcast)
    ctx = scene.tool_controller.ctx

    scene.tool_controller.on_press(QPointF(50, 50), Qt.MouseButton.LeftButton)
    item = next(i for i in scene.items() if isinstance(i, NumberItem))

    assert item.radius == ctx.stroke_width * NumberTool.RADIUS_SCALE


@pytest.mark.parametrize("broadcast", [1, 3, 7])
def test_the_panel_shows_what_will_actually_be_drawn(scene, qapp, broadcast):
    """这是原始 bug：面板显示 8，画出来却是 1 的大小。"""
    scene.activate_tool("number")
    scene.update_style(width=broadcast)

    panel = NumberSettingsPanel()
    panel.set_size(scene.tool_controller.ctx.stroke_width)

    scene.tool_controller.on_press(QPointF(50, 50), Qt.MouseButton.LeftButton)
    item = next(i for i in scene.items() if isinstance(i, NumberItem))

    assert item.radius == panel.size_spin.value() * NumberTool.RADIUS_SCALE
    assert panel.current_size == panel.size_spin.value()


def test_a_narrow_pen_does_not_drag_the_number_down(scene):
    """切到画笔调细，再切回序号，序号不该被带小。"""
    scene.activate_tool("pen")
    scene.update_style(width=1)
    assert scene.tool_controller.ctx.stroke_width == 1

    scene.activate_tool("number")
    assert scene.tool_controller.ctx.stroke_width >= NumberTool.MIN_WIDTH


def test_clamp_survives_rubbish_input(qapp):
    assert NumberTool.clamp_width(None) == NumberTool.MIN_WIDTH
    assert NumberTool.clamp_width("nonsense") == NumberTool.MIN_WIDTH
    assert NumberTool.clamp_width(-40) == NumberTool.MIN_WIDTH
    assert NumberTool.clamp_width(10 ** 9) == NumberTool.MAX_WIDTH
    # 基类同样受保护，只是范围更宽
    assert Tool.clamp_width(0) == Tool.MIN_WIDTH
    assert Tool.clamp_width(10 ** 9) == Tool.MAX_WIDTH
