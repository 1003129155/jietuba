"""文字的三态交互框，以及"编辑中点另一段文字"的单击切换。

矩形、椭圆、箭头早就有 _hovered + 虚线框，控制器也早就允许文字工具命中、切换文字；
缺的一直是文字这一侧：框由 Qt 自带的"选中就画虚线"顶着，分不出"正在编辑的这一段"
和"点一下就能切过去的那一段"；空文字框只有 6px 宽，四角按钮叠在一起；编辑中点到别的
文字，那一下只用来结束编辑，被整个吞掉。

这里按三件事分开锁：
1. 空文字框的最小交互宽度——只影响框、命中区和按钮，不影响字和背景；
2. 三种状态各画各的框，Qt 自带的高亮不再透出来；
3. 编辑中点另一段文字，一次点击就切过去。
"""
from math import ceil

import pytest
from PySide6.QtCore import QEvent, QPointF, QRectF, Qt
from PySide6.QtGui import QColor, QFont, QImage, QMouseEvent, QPainter
from PySide6.QtWidgets import QApplication, QStyle, QStyleOptionGraphicsItem

from canvas.handle_editor import HandleType, LayerEditor
from canvas.items import TextItem
from canvas.scene import CanvasScene
from canvas.view import CanvasView

# 框画在交互矩形往里 1px 处，线宽 2px：图最上面这几行正好罩住它，罩不到字。
# boundingRect() 现在比交互矩形多出一圈 CLICK_MARGIN 点击旷量，_render() 按
# boundingRect() 定图，框的位置相应往下挪了 CLICK_MARGIN 行，一并加上。
FRAME_BAND = 4 + TextItem.CLICK_MARGIN


def _text(text="note", pos=QPointF(0, 0), size=16):
    return TextItem(text, pos, QFont("Arial", size), QColor("black"))


@pytest.fixture
def canvas(qapp):
    """造一块选区已确认的画布；用例跑完按项目惯例收尾（留着不关会拖垮后面的用例）。"""
    created = []

    def make(tool="text"):
        image = QImage(300, 200, QImage.Format.Format_ARGB32_Premultiplied)
        image.fill(QColor("white"))
        scene = CanvasScene(image, QRectF(0, 0, 300, 200), enable_mosaic=True)
        view = CanvasView(scene)
        created.append((scene, view))
        # 场景不激活，图元的 hasFocus() 永远是 False，"正在编辑文字"这一路就走不到；
        # 真实窗口是显示并激活着的，这里补一个激活事件把测试放回同样的前提下。
        QApplication.sendEvent(view.viewport(), QEvent(QEvent.Type.WindowActivate))
        scene.selection_model.initialize_confirmed_rect(QRectF(0, 0, 300, 200))
        scene.tool_controller.activate(tool)
        return scene, view

    yield make

    for scene, view in created:
        view.close()
        scene.deleteLater()


def _render(item, option=None) -> QImage:
    """按包围盒出图，四周留 1px，好让画在边上的框整条都落在图里。"""
    rect = item.boundingRect()
    image = QImage(
        ceil(rect.width()) + 2,
        ceil(rect.height()) + 2,
        QImage.Format.Format_ARGB32_Premultiplied,
    )
    image.fill(Qt.GlobalColor.transparent)
    painter = QPainter(image)
    try:
        painter.translate(1 - rect.left(), 1 - rect.top())
        item.paint(painter, option if option is not None else QStyleOptionGraphicsItem(), None)
    finally:
        painter.end()
    return image


def _is_frame_pixel(color) -> bool:
    """框是亮蓝色，用例里的字是黑的、背景是白的，不会认错。"""
    return color.alpha() > 60 and color.blue() > 150 and color.red() < 120


def _top_edge_coverage(item, option=None):
    """(框的上边缘画到了多少列, 一共多少列)。

    实线满格、虚线有缺口、不画框是 0——一张图就把三种状态分开了。
    """
    image = _render(item, option)
    painted = [
        x
        for x in range(image.width())
        if any(_is_frame_pixel(image.pixelColor(x, y)) for y in range(FRAME_BAND))
    ]
    return len(painted), image.width()


def _mouse(view, scene_pos, kind):
    view_pos = QPointF(view.mapFromScene(scene_pos))
    press = kind == QEvent.Type.MouseButtonPress
    return QMouseEvent(
        kind,
        view_pos,
        view.mapToGlobal(view_pos.toPoint()),
        Qt.MouseButton.LeftButton,
        Qt.MouseButton.LeftButton if press else Qt.MouseButton.NoButton,
        Qt.KeyboardModifier.NoModifier,
    )


def _click(view, scene_pos):
    view.mousePressEvent(_mouse(view, scene_pos, QEvent.Type.MouseButtonPress))
    view.mouseReleaseEvent(_mouse(view, scene_pos, QEvent.Type.MouseButtonRelease))


# ======================================================================
# 最小交互宽度
# ======================================================================


def test_empty_text_box_is_wide_enough_for_its_corner_buttons(qapp):
    """空文字的文档区域只有 6px 宽，左上旋转和右上删除各 14px，按角点摆必然重叠。"""
    item = _text("")
    assert item.content_rect().width() < LayerEditor.FUNCTIONAL_HANDLE_SIZE
    assert item.interaction_rect().width() >= TextItem.MIN_INTERACTION_WIDTH

    rects = {handle.handle_type: handle.get_rect() for handle in item.get_edit_handles()}
    assert not rects[HandleType.ROTATE].intersects(rects[HandleType.ITEM_DELETE])


def test_the_widened_part_of_the_box_is_clickable(qapp):
    """框看得见却点不着，比不放宽更糟：命中区要跟着交互矩形走。

    QGraphicsTextItem.shape() 取的是它自己缓存的文档矩形，不会回头调用重写过的
    boundingRect()，所以 shape() 必须一起重写。
    """
    item = _text("")
    widened = QPointF(
        item.interaction_rect().right() - 2, item.content_rect().center().y()
    )

    assert not item.content_rect().contains(widened)
    assert item.contains(widened)


def test_widening_touches_neither_the_text_nor_its_background(qapp):
    """最小宽度只管交互：背景色块和导出的画面都不能跟着变宽。"""
    item = _text("1", size=10)
    content = item.content_rect()
    assert content.width() < TextItem.MIN_INTERACTION_WIDTH  # 前置条件：确实窄到要放宽

    item.set_background(True, QColor("red"), 255)
    image = _render(item)
    painted_right = max(
        x
        for x in range(image.width())
        for y in range(image.height())
        if image.pixelColor(x, y).alpha() > 0
    )

    # 包围盒的左上角落在图上的 (1, 1)，据此把内容矩形的右边换算到图坐标
    background_right = 1 - item.boundingRect().left() + content.right()
    assert painted_right <= background_right, "背景跟着交互矩形一起变宽了"


# ======================================================================
# 三态框
# ======================================================================


def test_the_frame_keeps_clear_of_the_blinking_caret(qapp):
    """框贴着光标画，闪起来看不出那一条是光标还是框。

    光标画在文字的起点（空文字）或末尾（打字时），离内容矩形只隔一个文档边距
    （3px），框还要再往里让 1px、线宽 2px——不额外让开，两条线就粘在一起。
    """
    margin = TextItem.TEXT_PADDING

    empty = _text("")
    caret_x = empty.content_rect().left() + margin
    assert caret_x - empty.interaction_rect().left() >= 6

    typed = _text("abc")
    caret_x = typed.content_rect().right() - margin
    assert typed.interaction_rect().right() - caret_x >= 6


def test_the_text_being_edited_gets_a_solid_frame(canvas):
    scene, view = canvas()
    item = _text("edit me")
    scene.addItem(item)

    view._enter_text_edit_mode(item)

    assert item.is_editing()
    painted, total = _top_edge_coverage(item)
    # -4 是原始容差（1px 渲染留白 * 两侧 + 2px 拐角抗锯齿宽松量）；现在
    # boundingRect() 比交互矩形每侧多留了 CLICK_MARGIN 点击旷量，图两侧
    # 各多出 CLICK_MARGIN 列必然不落在框上的空白，一起算进容差。
    tolerance = 4 + 2 * TextItem.CLICK_MARGIN
    assert painted >= total - tolerance, "编辑中的文字要一圈连续的实线框"


def test_hovering_a_candidate_text_gets_a_dashed_frame(canvas):
    """悬停的是"再点一下就会切过去"的那一段，和当前编辑的那一段必须一眼分得开。"""
    scene, view = canvas()
    item = _text("candidate")
    scene.addItem(item)

    item._set_hovered(True)

    painted, total = _top_edge_coverage(item)
    assert 0 < painted < total - 6, "候选文字要虚线框：画了，但有缺口"


def test_an_idle_text_has_no_frame(canvas):
    scene, view = canvas()
    item = _text("idle")
    scene.addItem(item)

    assert _top_edge_coverage(item)[0] == 0


def test_no_candidate_frame_when_the_tool_cannot_select_text(canvas):
    """矩形工具下点不中文字，就不该给它画"点一下能切过去"的框。"""
    scene, view = canvas(tool="rect")
    item = _text("candidate")
    scene.addItem(item)

    item._set_hovered(True)

    assert not item._hovered
    assert _top_edge_coverage(item)[0] == 0


def test_qt_builtin_highlight_does_not_show_through(qapp):
    """Qt 给选中/聚焦的文字画的那圈虚线分不出编辑和候选，还会和自己画的框叠成两圈。"""
    item = _text("idle")
    option = QStyleOptionGraphicsItem()
    option.state = QStyle.StateFlag.State_Selected | QStyle.StateFlag.State_HasFocus

    image = _render(item, option)

    leaked = [
        (x, y)
        for x in range(image.width())
        for y in range(3)
        if image.pixelColor(x, y).alpha() > 0
    ]
    assert not leaked, f"状态位没摘干净，Qt 自带的高亮又画出来了：{leaked[:5]}"


# ======================================================================
# 单击切换到另一段文字
# ======================================================================


def test_clicking_another_text_while_editing_switches_to_it(canvas):
    """编辑中点另一段文字，一次点击就切过去。

    以前这一下只用来"结束编辑"、随后被整个吞掉，要再点一次才选得中。
    """
    scene, view = canvas()
    first = _text("first", QPointF(10, 10))
    second = _text("second", QPointF(10, 120))
    scene.addItem(first)
    scene.addItem(second)
    view._enter_text_edit_mode(first)
    assert view._is_text_editing()

    _click(view, second.sceneBoundingRect().center())

    assert not first.is_editing()
    assert second.is_editing()
    assert view.smart_edit_controller.selected_item is second


def test_clicking_empty_space_while_editing_only_ends_the_edit(canvas):
    """空白处那一下仍然只确认编辑：不顺手再新建一段文字，也不画别的东西。"""
    scene, view = canvas()
    item = _text("first", QPointF(10, 10))
    scene.addItem(item)
    view._enter_text_edit_mode(item)

    _click(view, QPointF(250, 180))

    assert not item.is_editing()
    assert not view._is_text_editing()
    assert [i for i in scene.items() if isinstance(i, TextItem)] == [item]
