"""功能性手柄的统一外观测试。

手柄分两类：拖拽手柄改形状（缩放、圆角），功能性手柄按下去执行动作（旋转、
删除、序号加减）。后者必须共用一套画法——正方形、居中图样、图样四周到描边
之间填主题色——而不是每种功能各画各的。这里锁住这个约定，避免以后有人给某个
功能单独写一套渲染又漂移回去。
"""

import pytest
from PySide6.QtCore import QPointF, Qt
from PySide6.QtGui import QColor, QImage, QPainter

import core.theme as theme_module
from canvas.handle_editor import EditHandle, HandleType, LayerEditor


FUNCTIONAL_KINDS = [
    HandleType.ROTATE,
    HandleType.ITEM_DELETE,
    HandleType.NUMBER_DELETE,
    HandleType.NUMBER_INCREMENT,
    HandleType.NUMBER_DECREMENT,
]


@pytest.fixture
def theme(monkeypatch):
    """把主题色换掉，用完还原，避免污染同进程里的其它测试。"""
    instance = theme_module.get_theme()
    original = QColor(instance._theme_color)
    yield instance
    instance._theme_color = original


def _paint(handle, size=160):
    """把单个手柄放大画到图上，返回图像和手柄占据的中心坐标。"""
    image = QImage(size, size, QImage.Format.Format_ARGB32_Premultiplied)
    image.fill(QColor("white"))
    editor = LayerEditor()
    painter = QPainter(image)
    try:
        painter.translate(size / 2, size / 2)
        painter.scale(6, 6)
        editor._render_functional_handle(painter, handle)
    finally:
        painter.end()
    return image


def _handle(kind):
    return EditHandle(
        1,
        kind,
        QPointF(0, 0),
        Qt.CursorShape.PointingHandCursor,
        LayerEditor.FUNCTIONAL_HANDLE_SIZE,
        2,
    )


def test_every_action_handle_is_classified_as_functional(qapp):
    for kind in FUNCTIONAL_KINDS:
        assert kind in LayerEditor.FUNCTIONAL_HANDLE_TYPES

    # 改形状的手柄不能混进来，否则会被画成按钮
    for kind in (
        HandleType.TEXT_SCALE,
        HandleType.CORNER_BR,
        HandleType.EDGE_R,
        HandleType.CORNER_RADIUS,
    ):
        assert kind not in LayerEditor.FUNCTIONAL_HANDLE_TYPES


def test_all_functional_handles_share_one_size(qapp):
    """尺寸沿用序号按钮原本的大小，四个角看起来才是一套东西。"""
    assert LayerEditor.FUNCTIONAL_HANDLE_SIZE == LayerEditor.NUMBER_BUTTON_SIZE


@pytest.mark.parametrize("kind", FUNCTIONAL_KINDS)
def test_fill_around_the_glyph_uses_the_theme_colour(qapp, theme, kind):
    theme._theme_color = QColor("#1E88E5")
    image = _paint(_handle(kind))

    # 取图样与描边之间的一点：正方形半边长 7px、图样半径约 3.4px，
    # 放大 6 倍后 5px 处必然落在这段留白里。
    probe = image.pixelColor(int(image.width() / 2 + 5 * 6), int(image.height() / 2))
    assert (probe.red(), probe.green(), probe.blue()) == (0x1E, 0x88, 0xE5)


@pytest.mark.parametrize("kind", FUNCTIONAL_KINDS)
def test_fill_follows_a_theme_change(qapp, theme, kind):
    offset = int(5 * 6)

    theme._theme_color = QColor("#1E88E5")
    blue = _paint(_handle(kind)).pixelColor(80 + offset, 80)

    theme._theme_color = QColor("#43A047")
    green = _paint(_handle(kind)).pixelColor(80 + offset, 80)

    assert blue != green
    assert (green.red(), green.green(), green.blue()) == (0x43, 0xA0, 0x47)


def test_glyph_ink_flips_between_light_and_dark_themes(qapp, theme):
    """主题色是用户可改的，图样得跟着换黑白，不能写死一种颜色。"""
    light = LayerEditor._contrast_ink(QColor("#FFD54F"))
    dark = LayerEditor._contrast_ink(QColor("#C62828"))

    assert light.lightness() < dark.lightness(), "浅色底应该配深色图样，反之亦然"


@pytest.mark.parametrize("kind", FUNCTIONAL_KINDS)
def test_a_glyph_is_actually_drawn_inside_the_square(qapp, theme, kind):
    """只填色不画图样的话，四个手柄就长得一模一样、分不出功能。

    用浅色主题，图样会取深色，这样"深色像素"就只可能来自图样——底色和白色
    描边都不会被误计入。
    """
    theme._theme_color = QColor("#FFD54F")
    image = _paint(_handle(kind))

    half = int(LayerEditor.FUNCTIONAL_HANDLE_SIZE / 2 * 6)
    centre = image.width() // 2
    ink_pixels = sum(
        1
        for x in range(centre - half, centre + half)
        for y in range(centre - half, centre + half)
        if image.pixelColor(x, y).lightness() < 90
    )
    assert ink_pixels > 40, "方块里应当有可见的图样像素"
