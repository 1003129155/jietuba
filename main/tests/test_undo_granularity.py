"""一次用户操作，撤销栈里只进一条。

这是这个项目对撤销的基本约定："用户眼里的一步" = "Ctrl+Z 一次能退回的一步"。
它不是自动成立的，靠三套各司其职的机制撑着：

- 临时图元（TextItem._provisional）：新建的文字先不入栈，失焦时按内容决定是补一条
  AddItemCommand 还是当作没发生过。所以"点一下、打字、点空白确认"合起来是一条。
- 合并（NumberEditCommand.mergeWith）：0.7 秒内连点同一个序号的加减合并成一条，
  免得撤销栈被 1→2→3→4 这种琐碎步骤填满。
- 快照 + 终结（拖动、缩放）：进入拖动时抓一次状态，松手时比较前后、推一条
  EditItemCommand。中间移动多少次都不影响条数；原地按一下又放开则一条都不推。

历史上漏过一处：编辑中的文字用边缘拖走时，View 自己接管了移动（因为文字内部是
输入光标，只有边缘能拖），却没把撤销那一份接过来——位置变了，撤销栈里却什么都
没有，Ctrl+Z 会直接跳到更早的操作。

下面逐个操作数条数。加新工具、新交互时在 CASES 里补一行，粒度跑偏会立刻响。
"""
import pytest
from PySide6.QtCore import QEvent, QPointF, QRectF, Qt
from PySide6.QtGui import QColor, QImage, QMouseEvent, QPen
from PySide6.QtWidgets import QApplication

from canvas.items import NumberItem, RectItem, TextItem
from canvas.scene import CanvasScene
from canvas.undo import AddItemCommand
from canvas.view import CanvasView

W, H = 500, 380
RED = QColor("#FF3B30")


@pytest.fixture
def canvas(qapp):
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


def _send(view, kind, pos, buttons=Qt.MouseButton.LeftButton):
    view_pos = QPointF(view.mapFromScene(pos))
    event = QMouseEvent(
        kind,
        view_pos,
        view.mapToGlobal(view_pos.toPoint()),
        Qt.MouseButton.LeftButton,
        buttons,
        Qt.KeyboardModifier.NoModifier,
    )
    if kind == QEvent.Type.MouseButtonPress:
        view.mousePressEvent(event)
    elif kind == QEvent.Type.MouseMove:
        view.mouseMoveEvent(event)
    else:
        view.mouseReleaseEvent(event)
    QApplication.processEvents()


def _gesture(view, start, end, steps=5):
    """一次完整的按下—拖—松开。中间走多少步都不该影响入栈条数。"""
    _send(view, QEvent.Type.MouseButtonPress, start)
    for i in range(1, steps + 1):
        _send(view, QEvent.Type.MouseMove, start + (end - start) * (i / steps))
    _send(view, QEvent.Type.MouseButtonRelease, end, Qt.MouseButton.NoButton)


def _put_rect(scene):
    item = RectItem(QRectF(60, 60, 140, 90), QPen(RED, 4))
    scene.addItem(item)
    scene.undo_stack.push(AddItemCommand(scene, item))
    return item


def _write_text(scene, view, text="hello"):
    """走真实路径建一段文字：点一下画布，TextTool 建出图元并 setFocus。"""
    _gesture(view, QPointF(120, 120), QPointF(120, 120), steps=1)
    item = next(i for i in scene.items() if isinstance(i, TextItem))
    item.setPlainText(text)
    QApplication.processEvents()
    return item


def _drag_handle_point(item):
    """文字框下边缘的中点——编辑态下抓这里拖动整段文字。

    取下边缘而不是左右边缘的中点：那两处是文本行的垂直中心，按下去归文本光标定位
    管；下边缘中部离三个角的功能手柄也最远。
    """
    rect = item.mapToScene(item.boundingRect()).boundingRect()
    return QPointF(rect.center().x(), rect.bottom() - 2)


# 名称 -> (工具, 前置准备, 被测量的那一次操作)
CASES = {
    "画一个矩形": ("rect", None, lambda s, v: _gesture(v, QPointF(60, 60), QPointF(200, 160))),
    "画一条画笔笔画": ("pen", None, lambda s, v: _gesture(v, QPointF(60, 60), QPointF(220, 170), 12)),
    "画一个箭头": ("arrow", None, lambda s, v: _gesture(v, QPointF(60, 60), QPointF(220, 170))),
    "涂一片马赛克": ("mosaic", None, lambda s, v: _gesture(v, QPointF(60, 60), QPointF(220, 170), 10)),
    "点一个序号": ("number", None, lambda s, v: _gesture(v, QPointF(120, 120), QPointF(120, 120), 1)),
    "写一段文字": ("text", None, lambda s, v: (_write_text(s, v), v._end_text_edit(
        next(i for i in s.items() if isinstance(i, TextItem))))),
    "拖动一个矩形": ("rect", lambda s, v: _put_rect(s),
                lambda s, v: _gesture(v, QPointF(60, 105), QPointF(160, 175), 6)),
    "删除选中的矩形": ("rect", lambda s, v: v.smart_edit_controller.select_item(_put_rect(s)),
                 lambda s, v: v.smart_edit_controller.delete_selected()),
}


@pytest.mark.parametrize("name", sorted(CASES))
def test_one_gesture_pushes_exactly_one_command(canvas, name):
    tool, setup, action = CASES[name]
    scene, view = canvas(tool)
    if setup:
        setup(scene, view)
        QApplication.processEvents()

    before = scene.undo_stack.count()
    action(scene, view)
    QApplication.processEvents()

    assert scene.undo_stack.count() - before == 1, f"{name}：这一步该恰好入栈一条"


def test_dragging_text_while_editing_can_be_undone(canvas):
    """编辑中的文字被拖走之后，Ctrl+Z 要能把它放回去。

    这条路 View 自己接管移动（文字内部是输入光标，只有边缘能拖），从前接管了移动
    却没接管撤销：位置变了，栈里什么都没有。
    """
    scene, view = canvas("text")
    item = _write_text(scene, view)
    assert item.is_editing(), "前置条件：文字此刻在编辑态"

    start = QPointF(item.pos())
    before = scene.undo_stack.count()
    edge = _drag_handle_point(item)
    _gesture(view, edge, edge + QPointF(60, 45), steps=4)

    assert view.text_drag.active is False, "松手后拖动状态该清掉"
    assert QPointF(item.pos()) != start, "前置条件：它确实被拖走了"
    assert scene.undo_stack.count() - before == 1, "拖动该恰好入栈一条"

    scene.undo_stack.undo()
    QApplication.processEvents()
    assert QPointF(item.pos()) == start, "撤销没把它放回原处"


def test_dragging_text_after_editing_can_be_undone(canvas):
    """退出编辑后再拖，走的是普通图元那条路，同样是一条。"""
    scene, view = canvas("text")
    item = _write_text(scene, view)
    view._end_text_edit(item)
    QApplication.processEvents()
    assert not item.is_editing()

    start = QPointF(item.pos())
    before = scene.undo_stack.count()
    edge = _drag_handle_point(item)
    _gesture(view, edge, edge + QPointF(60, 45), steps=4)

    assert QPointF(item.pos()) != start
    assert scene.undo_stack.count() - before == 1
    scene.undo_stack.undo()
    QApplication.processEvents()
    assert QPointF(item.pos()) == start


def test_pressing_without_moving_leaves_no_trace(canvas):
    """原地按一下又放开，不该在历史里留一条空记录。

    快照是按下就抓的，靠 _finalize_move_edit 比较前后状态把这种空操作挡掉。
    """
    scene, view = canvas("rect")
    item = _put_rect(scene)
    view.smart_edit_controller.select_item(item)
    QApplication.processEvents()

    before = scene.undo_stack.count()
    spot = QPointF(60, 105)
    _send(view, QEvent.Type.MouseButtonPress, spot)
    _send(view, QEvent.Type.MouseButtonRelease, spot, Qt.MouseButton.NoButton)

    assert scene.undo_stack.count() == before, "原地点一下也进了历史"


def test_repeated_number_edits_collapse_into_one(canvas):
    """连点同一个序号的加减：短时间内的连续改动合并成一条。"""
    from canvas.undo import NumberEditCommand

    scene, view = canvas("number")
    item = NumberItem(1, QPointF(150, 150), 20.0, RED)
    scene.addItem(item)
    scene.undo_stack.push(AddItemCommand(scene, item))

    before = scene.undo_stack.count()
    for n in (2, 3, 4):
        old = {"number": item.number}
        item.number = n
        scene.undo_stack.push(NumberEditCommand(item, old, {"number": n}))
        QApplication.processEvents()

    assert scene.undo_stack.count() - before == 1, "三次连改该并成一条"


def test_a_long_drag_is_still_one_step(canvas):
    """拖动过程中鼠标动了几十次，仍然只有一条——快照只在进入拖动时抓一次。"""
    scene, view = canvas("rect")
    _put_rect(scene)

    before = scene.undo_stack.count()
    _gesture(view, QPointF(60, 105), QPointF(260, 275), steps=40)

    assert scene.undo_stack.count() - before == 1


def test_text_written_in_one_go_is_one_step(canvas):
    """新建、打字、点空白确认——三个动作，用户眼里是"写了一段字"，所以是一条。"""
    scene, view = canvas("text")
    before = scene.undo_stack.count()

    item = _write_text(scene, view, "some words")
    view._end_text_edit(item)
    QApplication.processEvents()

    assert scene.undo_stack.count() - before == 1
    scene.undo_stack.undo()
    QApplication.processEvents()
    assert not [i for i in scene.items() if isinstance(i, TextItem)], "撤销该把整段字收走"
