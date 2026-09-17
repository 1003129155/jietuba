"""候选/选中框在各图元上的统一行为。

这套"鼠标停上去画一圈框"的状态机，以前在矩形、椭圆、箭头、序号里各复制了一份
（四份逐字节相同），文字和框选马赛克则整个漏掉——加图元类型时候选反馈不会自动
跟过来，抄漏了也不报错、不挂测试，只是少一圈框。现在它归 DrawingItemMixin，
这里就按图元类型逐个跑同一组断言：再加新图元时，在 CANDIDATES 里补一行就知道
它接没接上。

框有两种含义，只用线型区分：实线那一段是四角按钮此刻作用的对象，虚线那一段是
鼠标再点一下就会切过去的候选。这个区分原先只有文字有，其余图元选中和悬停画的
是同一种虚线，同屏时看不出手柄归谁。

顺带锁住两件容易悄悄回退的事：
- mixin 必须排在 Qt 基类前面。QGraphicsItem 自己也定义了 hoverEnterEvent，写在
  后面会被 MRO 挡住，mixin 里的实现根本轮不到执行。所以这里的悬停走真实的鼠标
  事件派发，而不是直接调处理器——挡住了就挂。
- 序号只画一圈框：以前图元画一圈、LayerEditor 又画一圈，两圈还差 6px。
"""
from math import ceil

import pytest
from PySide6.QtCore import QEvent, QPointF, QRectF, Qt
from PySide6.QtGui import (
    QColor,
    QFont,
    QImage,
    QMouseEvent,
    QPainter,
    QPainterPath,
    QPen,
)
from PySide6.QtWidgets import QApplication, QStyleOptionGraphicsItem

from canvas.items import (
    ArrowItem,
    EllipseItem,
    MosaicItem,
    NumberItem,
    RectItem,
    StrokeItem,
    TextItem,
)
from canvas.scene import CanvasScene
from canvas.view import CanvasView

RED = QColor("#FF3B30")


@pytest.fixture
def canvas(qapp):
    """造一块选区已确认的画布；用例跑完按项目惯例收尾。"""
    created = []

    def make(tool):
        image = QImage(300, 220, QImage.Format.Format_ARGB32_Premultiplied)
        image.fill(QColor("white"))
        scene = CanvasScene(image, QRectF(0, 0, 300, 220), enable_mosaic=True)
        view = CanvasView(scene)
        created.append((scene, view))
        scene.selection_model.initialize_confirmed_rect(QRectF(0, 0, 300, 220))
        scene.tool_controller.activate(tool)
        return scene, view

    yield make

    for scene, view in created:
        view.close()
        scene.deleteLater()


def _mosaic(fill_mode):
    """马赛克的"颜料"用纯灰，免得纹理里混进蓝色像素被当成框。"""
    reduced = QImage(6, 6, QImage.Format.Format_ARGB32_Premultiplied)
    reduced.fill(QColor("#808080"))
    path = QPainterPath()
    if fill_mode:
        path.addRect(QRectF(40, 40, 80, 60))
    else:
        path.moveTo(QPointF(40, 70))
        path.lineTo(QPointF(120, 70))
    return MosaicItem(path, 16, 8, reduced, QRectF(0, 0, 300, 220), fill_mode=fill_mode)


def _arrow():
    return ArrowItem(QPointF(40, 40), QPointF(140, 40), QPen(RED, 6))


def _stroke():
    path = QPainterPath(QPointF(40, 70))
    path.lineTo(QPointF(140, 70))
    return StrokeItem(path, QPen(RED, 8))


# 图元类型 -> (工具, 造图元, 悬停点：必须落在图元自己的命中区上)
CANDIDATES = {
    "rect": ("rect", lambda: RectItem(QRectF(40, 40, 80, 60), QPen(RED, 4)), QPointF(40, 70)),
    "ellipse": ("ellipse", lambda: EllipseItem(QRectF(40, 40, 80, 60), QPen(RED, 4)), QPointF(40, 70)),
    "arrow": ("arrow", _arrow, QPointF(90, 40)),
    "number": ("number", lambda: NumberItem(1, QPointF(80, 80), 18.0, RED), QPointF(80, 80)),
    "mosaic": ("mosaic", lambda: _mosaic(True), QPointF(80, 70)),
    "text": ("text", lambda: TextItem("abc", QPointF(40, 40), QFont("Arial", 20), QColor("black")), None),
}


def _is_frame_pixel(color) -> bool:
    """框是亮蓝色；用例里的图元都画成红/黑/灰，不会认错。"""
    return color.alpha() > 60 and color.blue() > 150 and color.red() < 120


def _render(item) -> QImage:
    """按包围盒出图，四周留 2px，好让画在边上的框整条都落在图里。"""
    rect = item.boundingRect()
    image = QImage(
        ceil(rect.width()) + 4,
        ceil(rect.height()) + 4,
        QImage.Format.Format_ARGB32_Premultiplied,
    )
    image.fill(Qt.GlobalColor.transparent)
    painter = QPainter(image)
    try:
        painter.translate(2 - rect.left(), 2 - rect.top())
        item.paint(painter, QStyleOptionGraphicsItem(), None)
    finally:
        painter.end()
    return image


def _frame_painted(item) -> bool:
    image = _render(item)
    return any(
        _is_frame_pixel(image.pixelColor(x, y))
        for y in range(image.height())
        for x in range(image.width())
    )


def _longest_frame_run(item) -> int:
    """整张图里最长的一段连续框像素。

    实线的边是一整条，虚线被缺口切成一小段一小段，两者差着一个量级。线型是这
    套反馈的全部区别，得真的落到像素上——只断言状态机的话，绘制那一侧把线型丢了
    也照样过。
    """
    image = _render(item)
    best = 0
    for y in range(image.height()):
        run = 0
        for x in range(image.width()):
            if _is_frame_pixel(image.pixelColor(x, y)):
                run += 1
                best = max(best, run)
            else:
                run = 0
    return best


def _hover(view, scene_pos):
    """走真实的鼠标移动，让 Qt 自己把悬停事件派发到图元上。"""
    pos = QPointF(view.mapFromScene(scene_pos))
    view.mouseMoveEvent(
        QMouseEvent(
            QEvent.Type.MouseMove,
            pos,
            view.mapToGlobal(pos.toPoint()),
            Qt.MouseButton.NoButton,
            Qt.MouseButton.NoButton,
            Qt.KeyboardModifier.NoModifier,
        )
    )
    QApplication.processEvents()


@pytest.mark.parametrize("kind", sorted(CANDIDATES))
def test_hovering_a_candidate_paints_a_frame(canvas, kind):
    tool, factory, hover_pos = CANDIDATES[kind]
    scene, view = canvas(tool)
    item = factory()
    scene.addItem(item)
    if hover_pos is None:
        hover_pos = item.sceneBoundingRect().center()

    assert not item._hovered
    assert not _frame_painted(item), f"{kind}：没人碰它的时候不该有框"

    _hover(view, hover_pos)

    assert item._hovered, f"{kind}：悬停事件没派发到图元（mixin 被 MRO 挡住了？）"
    assert _frame_painted(item), f"{kind}：悬停时要画出候选框"


@pytest.mark.parametrize("kind", sorted(CANDIDATES))
def test_leaving_takes_the_frame_away(canvas, kind):
    tool, factory, hover_pos = CANDIDATES[kind]
    scene, view = canvas(tool)
    item = factory()
    scene.addItem(item)
    if hover_pos is None:
        hover_pos = item.sceneBoundingRect().center()

    _hover(view, hover_pos)
    _hover(view, QPointF(280, 200))

    assert not item._hovered
    assert not _frame_painted(item)


@pytest.mark.parametrize("kind", ["stroke", "mosaic"])
def test_freehand_marks_are_not_candidates(canvas, kind):
    """自由笔画（画笔、涂抹马赛克）要 Ctrl 才选得中，鼠标经过不该有任何反馈。

    统一之后最容易顺手改掉的就是这条：它们和框选马赛克共用同一套代码，
    区别只在 can_show_hover_cursor 对 PATH 返回 False。
    """
    tool = "pen" if kind == "stroke" else "mosaic"
    scene, view = canvas(tool)
    item = _stroke() if kind == "stroke" else _mosaic(False)
    scene.addItem(item)

    _hover(view, QPointF(90, 70))

    assert not item._hovered
    assert not _frame_painted(item)


@pytest.mark.parametrize("kind", sorted(CANDIDATES))
def test_the_current_object_gets_a_solid_frame(canvas, kind):
    """选中的那个画实线：手柄此刻挂在它身上，和"再点一下会切过去"的候选得分得开。"""
    tool, factory, hover_pos = CANDIDATES[kind]
    scene, view = canvas(tool)
    item = factory()
    scene.addItem(item)
    if hover_pos is None:
        hover_pos = item.sceneBoundingRect().center()

    view.smart_edit_controller.select_item(item)
    assert item.selection_frame_style() == Qt.PenStyle.SolidLine, f"{kind}：当前对象要实线框"

    _hover(view, hover_pos)
    assert item.selection_frame_style() == Qt.PenStyle.SolidLine, (
        f"{kind}：鼠标停上去把当前对象的实线顶成了虚线"
    )


def test_the_solid_frame_is_really_solid(canvas):
    """线型得真的落到像素上：只断言状态机的话，绘制那一侧把它丢了照样过。"""
    scene, view = canvas("rect")
    item = RectItem(QRectF(40, 40, 80, 60), QPen(RED, 4))
    scene.addItem(item)

    _hover(view, QPointF(40, 70))
    dashed_run = _longest_frame_run(item)

    view.smart_edit_controller.select_item(item)
    solid_run = _longest_frame_run(item)

    # 实线是一整条边，虚线被缺口切成一小段一小段，差着一个量级
    assert 0 < dashed_run < solid_run / 3


@pytest.mark.parametrize("kind", ["stroke", "mosaic"])
def test_freehand_marks_stay_frameless_even_when_selected(canvas, kind):
    """自由笔画被 Ctrl 选中后也没有框：实线这一档同样要过"当前工具选得中它"这关。

    它们和框选马赛克共用同一套代码，"选中就画实线"要是漏掉这道门，画笔笔画和涂抹
    马赛克上就会平白多出一圈框。
    """
    tool = "pen" if kind == "stroke" else "mosaic"
    scene, view = canvas(tool)
    item = _stroke() if kind == "stroke" else _mosaic(False)
    scene.addItem(item)

    view.smart_edit_controller.select_item(item)

    assert item.is_edit_target(), f"{kind}：前置条件——控制器确实选中了它"
    assert not item.shows_selection_frame()
    assert item.selection_frame_style() is None
    assert not _frame_painted(item)


def _scene_image(scene, view) -> QImage:
    image = QImage(300, 220, QImage.Format.Format_ARGB32_Premultiplied)
    image.fill(QColor("white"))
    painter = QPainter(image)
    try:
        scene.selection_item.setVisible(False)
        scene.render(painter, QRectF(0, 0, 300, 220), QRectF(0, 0, 300, 220))
    finally:
        painter.end()
    return image


@pytest.mark.parametrize("kind", ["stroke", "mosaic"])
def test_qt_does_not_highlight_a_selected_freehand_mark(canvas, kind):
    """Qt 自带的选中高亮不能漏出来。

    QGraphicsPathItem.paint() 在 option.state 带 State_Selected 时会自己画一圈
    boundingRect 的虚线矩形——对一条斜的笔画就是套一个大方框，既不是我们的三态
    语义，也丑。图元不设 ItemIsSelectable，Qt 那份选中状态就永远是 False，这条
    路径走不到；谁要是把那个标志加回来，这里就会挂。
    """
    tool = "pen" if kind == "stroke" else "mosaic"
    scene, view = canvas(tool)
    item = _stroke() if kind == "stroke" else _mosaic(False)
    scene.addItem(item)

    before = _scene_image(scene, view)
    view.smart_edit_controller.select_item(item)
    after = _scene_image(scene, view)

    assert item.is_edit_target(), "前置条件：控制器确实选中了它"
    assert not item.isSelected(), "Qt 不该持有选中状态——选中只有控制器一个所有者"
    assert after == before, "选中之后画面变了：Qt 自带的高亮漏出来了"


def test_the_number_has_exactly_one_frame(canvas):
    """以前图元自己画一圈、LayerEditor 又画一圈，选中的序号上有两个虚线框。"""
    scene, view = canvas("number")
    item = NumberItem(1, QPointF(150, 110), 18.0, RED)
    scene.addItem(item)
    view.smart_edit_controller.select_item(item)

    image = QImage(300, 220, QImage.Format.Format_ARGB32_Premultiplied)
    image.fill(QColor("white"))
    painter = QPainter(image)
    try:
        scene.selection_item.setVisible(False)
        scene.render(painter, QRectF(0, 0, 300, 220), QRectF(0, 0, 300, 220))
        view.smart_edit_controller.layer_editor.render(painter)
    finally:
        painter.end()

    inner = item.sceneVisualRect()
    outer = item.sceneBoundingRect()  # 手柄挂在它的角上，以前多出来的那圈也画在这里

    def has_frame(x_center):
        # 圆形下半部分：那几行上没有 +/-/× 按钮，只可能是框
        return any(
            _is_frame_pixel(image.pixelColor(x, y))
            for y in range(int(inner.bottom()) - 8, int(inner.bottom()) - 1)
            for x in range(int(x_center) - 1, int(x_center) + 2)
        )

    assert has_frame(inner.left()) and has_frame(inner.right()), "序号自己那圈框没画出来"
    assert not has_frame(outer.left()) and not has_frame(outer.right()), (
        "外面还有一圈框：LayerEditor 又画了一遍"
    )
