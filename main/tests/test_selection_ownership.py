"""选中状态归谁管，以及这件事牵动的两根线：拖动和 Qt 的默认绘制。

选中曾经有两个所有者：SmartEditController 记一份 selected_item，又用
setSelected() 往 Qt 那边同步一份。Qt 自己在鼠标事件里也会改它——
QGraphicsItem::mouseReleaseEvent 撞上 Ctrl 会把选中翻转——于是 Ctrl 选中画笔
笔画时，框闪一下就没了，而控制器那边还以为它选着。

现在只剩一个所有者：selected_item。图元画框问控制器，Qt 那份状态没人写也没人读，
ItemIsSelectable 索性不设。拖动仍然走 Qt 的 ItemIsMovable：它不看选中，图元照样
被 scene 认成 mouse grabber——但这是实测出来的，不是显然的，所以下面第 1 组用例
逐个图元把它钉住（改造前整套 1817 个用例里没有一条走过真实鼠标拖动）。

这里就锁这套契约的四个面：
1. 各类图元都还能用真实鼠标事件拖动；
2. Qt 不持有选中状态，它那圈自带高亮也就无从画起；
3. 新建的文字只 setFocus() 不进控制器，照样有实线框；
4. 选中和取消选中都要让图元重画，否则框不会跟着变。
"""
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
from PySide6.QtWidgets import QApplication
import pytest

from canvas.items import (
    ArrowItem,
    EllipseItem,
    MosaicItem,
    NumberItem,
    RectItem,
    StrokeItem,
    TextItem,
)
from canvas.items.spotlight_item import SpotlightItem
from canvas.scene import CanvasScene
from canvas.view import CanvasView

RED = QColor("#FF3B30")
W, H = 400, 300
BOX = QRectF(80, 80, 140, 60)


@pytest.fixture
def canvas(qapp):
    """一块选区已确认的画布；用例跑完按项目惯例收尾。"""
    created = []

    def make(tool):
        image = QImage(W, H, QImage.Format.Format_ARGB32_Premultiplied)
        image.fill(QColor("white"))
        scene = CanvasScene(image, QRectF(0, 0, W, H), enable_mosaic=True)
        view = CanvasView(scene)
        created.append((scene, view))
        # 场景不激活，图元的 hasFocus() 永远是 False，"正在编辑文字"那一路走不到
        QApplication.sendEvent(view.viewport(), QEvent(QEvent.Type.WindowActivate))
        scene.selection_model.initialize_confirmed_rect(QRectF(0, 0, W, H))
        scene.tool_controller.activate(tool)
        return scene, view

    yield make

    for scene, view in created:
        view.close()
        scene.deleteLater()


def _stroke(highlighter=False):
    path = QPainterPath(QPointF(80, 100))
    path.lineTo(QPointF(220, 130))
    return StrokeItem(path, QPen(RED, 10), is_highlighter=highlighter)


def _mosaic(fill_mode):
    reduced = QImage(8, 8, QImage.Format.Format_ARGB32_Premultiplied)
    reduced.fill(QColor("#808080"))
    path = QPainterPath()
    if fill_mode:
        path.addRect(BOX)
    else:
        path.moveTo(QPointF(80, 110))
        path.lineTo(QPointF(220, 110))
    return MosaicItem(path, 16, 8, reduced, QRectF(0, 0, W, H), fill_mode=fill_mode)


# 图元 -> (工具, 造图元, 抓取点, 要不要 Ctrl)
DRAGGABLE = {
    "stroke": ("pen", lambda: _stroke(False), QPointF(150, 116), True),
    "highlighter": ("highlighter", lambda: _stroke(True), QPointF(150, 116), True),
    "mosaic_free": ("mosaic", lambda: _mosaic(False), QPointF(150, 110), True),
    "mosaic_box": ("mosaic", lambda: _mosaic(True), QPointF(150, 110), False),
    "rect": ("rect", lambda: RectItem(BOX, QPen(RED, 4)), QPointF(80, 110), False),
    "ellipse": ("ellipse", lambda: EllipseItem(BOX, QPen(RED, 4)), QPointF(80, 110), False),
    "arrow": ("arrow", lambda: ArrowItem(QPointF(80, 100), QPointF(220, 130), QPen(RED, 6)),
              QPointF(150, 116), False),
    "number": ("number", lambda: NumberItem(1, QPointF(150, 110), 20.0, RED), QPointF(150, 110), False),
    "spotlight": ("spotlight", lambda: SpotlightItem(BOX), QPointF(150, 110), False),
}

DELTA = QPointF(40, 30)


def _send(view, kind, scene_pos, buttons, modifiers):
    view_pos = QPointF(view.mapFromScene(scene_pos))
    event = QMouseEvent(
        kind,
        view_pos,
        view.mapToGlobal(view_pos.toPoint()),
        Qt.MouseButton.LeftButton,
        buttons,
        modifiers,
    )
    if kind == QEvent.Type.MouseButtonPress:
        view.mousePressEvent(event)
    elif kind == QEvent.Type.MouseMove:
        view.mouseMoveEvent(event)
    else:
        view.mouseReleaseEvent(event)
    QApplication.processEvents()


def _drag(view, start, delta, modifiers):
    held = Qt.MouseButton.LeftButton
    _send(view, QEvent.Type.MouseButtonPress, start, held, modifiers)
    # 分两步挪，跨过可能存在的起步阈值
    _send(view, QEvent.Type.MouseMove, start + delta / 2, held, modifiers)
    _send(view, QEvent.Type.MouseMove, start + delta, held, modifiers)
    _send(view, QEvent.Type.MouseButtonRelease, start + delta, Qt.MouseButton.NoButton, modifiers)


@pytest.mark.parametrize("kind", sorted(DRAGGABLE))
def test_items_can_still_be_dragged_with_the_mouse(canvas, kind):
    """图元得拖得动。

    拖动靠 Qt 的 ItemIsMovable，而 Qt 只把 move 派给 mouse grabber。选中的所有权
    收归控制器之后 ItemIsSelectable 就不设了，"不可选中的图元还能不能当 grabber"
    于是成了拖动的前提——它确实还能，但这是实测结论，往后谁再动图元的 flag 或者
    事件路径，这里会第一个响。
    """
    tool, factory, grab, need_ctrl = DRAGGABLE[kind]
    scene, view = canvas(tool)
    item = factory()
    scene.addItem(item)
    start = QPointF(item.pos())

    modifiers = Qt.KeyboardModifier.ControlModifier if need_ctrl else Qt.KeyboardModifier.NoModifier
    _drag(view, grab, DELTA, modifiers)

    moved = QPointF(item.pos()) - start
    assert (moved.x(), moved.y()) == (DELTA.x(), DELTA.y()), f"{kind}：没跟着鼠标走"


def test_both_drag_paths_behave_the_same(canvas):
    """拖动有两条路径，可观察的行为必须一样。

    Qt 默认把事件交给 z 轴最上方的图元，而控制器有时要越过它往下找当前工具选得中
    的那个（拿矩形工具点一段压在矩形边框上的文字时就是如此）。那种情况下 View 自己
    接管拖动（press_requires_manual_dispatch），其余交给 Qt 的 ItemIsMovable。

    这不是两套实现：位置谁来改是分工，而"拖动前抓快照、松手时推 EditItemCommand"
    只有 handle_move/_finalize_move_edit 这一套，所以两条路径的位移、入栈、撤销、
    重做都该对得上。这里把它锁住——哪条路径以后跑偏了都会响。
    """
    scene, view = canvas("rect")
    rect = RectItem(BOX, QPen(RED, 4))
    scene.addItem(rect)
    # 文字压在矩形上边框上：点那儿，顶层是文字，而矩形工具只选得中矩形
    text = TextItem("overlap", QPointF(78, 62), QFont("Arial", 22), QColor("black"))
    scene.addItem(text)

    on_border = QPointF(120, BOX.top())
    hits = [i for i in scene.items(on_border) if isinstance(i, (RectItem, TextItem))]
    assert isinstance(hits[0], TextItem), "前置条件：点击处顶层得是文字"
    assert any(isinstance(i, RectItem) for i in hits), "前置条件：底下得压着矩形"

    start = QPointF(rect.pos())
    text_start = QPointF(text.pos())
    depth = scene.undo_stack.count()

    _drag(view, on_border, DELTA, Qt.KeyboardModifier.NoModifier)

    assert QPointF(text.pos()) == text_start, "顶层的文字不该被拖走"
    moved = QPointF(rect.pos()) - start
    assert (moved.x(), moved.y()) == (DELTA.x(), DELTA.y()), "底下的矩形没跟着走"
    assert scene.undo_stack.count() - depth == 1, "这一次拖动只该入栈一条命令"

    scene.undo_stack.undo()
    QApplication.processEvents()
    assert QPointF(rect.pos()) == start, "撤销没把它放回原处"


@pytest.mark.parametrize("kind", sorted(DRAGGABLE))
def test_qt_never_owns_the_selection(canvas, kind):
    """选中只有控制器一个所有者，Qt 那份状态自始至终是空的。

    只要图元带着 ItemIsSelectable，Qt 就会在自己的鼠标事件里改选中：非 Ctrl
    的按下会 setSelected(true)，带 Ctrl 的松开会把它翻转——后者正是"Ctrl 选中
    画笔笔画，框闪一下就没"的成因。
    """
    tool, factory, grab, need_ctrl = DRAGGABLE[kind]
    scene, view = canvas(tool)
    item = factory()
    scene.addItem(item)

    modifiers = Qt.KeyboardModifier.ControlModifier if need_ctrl else Qt.KeyboardModifier.NoModifier
    held = Qt.MouseButton.LeftButton

    _send(view, QEvent.Type.MouseButtonPress, grab, held, modifiers)
    assert not item.isSelected(), f"{kind}：按下之后 Qt 把选中记到自己身上了"
    _send(view, QEvent.Type.MouseButtonRelease, grab, Qt.MouseButton.NoButton, modifiers)
    assert not item.isSelected(), f"{kind}：松开之后 Qt 把选中记到自己身上了"

    # 控制器那边才是真相：点在图元上就该选中它
    assert view.smart_edit_controller.selected_item is item, f"{kind}：控制器没选中它"


@pytest.mark.parametrize("kind", ["stroke", "mosaic_free"])
def test_selecting_a_freehand_mark_changes_nothing_on_screen(canvas, kind):
    """自由笔画被选中时，画面一个像素都不该变。

    它们不画自己的框（shows_selection_frame 为 False），Qt 的自带高亮又因为
    isSelected() 恒为 False 而画不出来——两道保险，这里一起锁住。
    """
    tool, factory, _, _ = DRAGGABLE[kind]
    scene, view = canvas(tool)
    item = factory()
    scene.addItem(item)

    def shot():
        image = QImage(W, H, QImage.Format.Format_ARGB32_Premultiplied)
        image.fill(QColor("white"))
        painter = QPainter(image)
        try:
            scene.selection_item.setVisible(False)
            scene.render(painter, QRectF(0, 0, W, H), QRectF(0, 0, W, H))
        finally:
            painter.end()
        return image

    before = shot()
    view.smart_edit_controller.select_item(item)
    after = shot()

    assert item.is_edit_target(), "前置条件：控制器确实选中了它"
    assert after == before, "选中之后画面变了"


def test_a_newly_typed_text_gets_a_solid_frame_without_the_controller(canvas):
    """刚建出来的文字只 setFocus()，不经过控制器，照样要有实线框。

    TextTool 就是这么建的（见 tools/text.py）：此刻 selected_item 还是空的，
    框全靠 TextItem 自己那条编辑态分支撑着。
    """
    scene, view = canvas("text")
    item = TextItem("hi", QPointF(80, 80), QFont("Arial", 20), QColor("black"))
    scene.addItem(item)

    item.setFocus()

    assert view.smart_edit_controller.selected_item is None, "前置条件：控制器并不知道它"
    assert item.is_editing()
    assert item.is_edit_target(), "正在编辑的那一段就是当前对象"
    assert item.selection_frame_style() == Qt.PenStyle.SolidLine


def test_selecting_and_deselecting_repaint_the_item(canvas):
    """选中和取消选中都要让图元重画。

    框的有无现在只由 selected_item 决定，Qt 那边没有状态变化可以顺带触发重绘，
    控制器必须自己 update()——否则框要等到下一次别的原因重绘才跟上。
    """
    scene, view = canvas("rect")
    item = RectItem(BOX, QPen(RED, 4))
    scene.addItem(item)
    QApplication.processEvents()

    regions = []
    scene.changed.connect(lambda rects: regions.extend(rects))

    view.smart_edit_controller.select_item(item)
    QApplication.processEvents()
    assert regions, "选中之后没有任何重绘"

    regions.clear()
    view.smart_edit_controller.clear_selection()
    QApplication.processEvents()
    assert regions, "取消选中之后没有任何重绘"
