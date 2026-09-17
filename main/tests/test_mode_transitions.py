"""一种模式结束、切到另一种时，上一种的状态要清干净。

CanvasView 的鼠标事件是按模式分发的（见 mouseMoveEvent 的路由）：拉选区、绘图、
文字拖动、编辑模式、悬停预览，五者互斥。判断走哪一种，靠的是散在 view 和控制器上
的一排标志。所以"上一种模式的标志有没有留下"就是这套分发的命门——留一个下来，下次
鼠标一动就会拐进错误的分支：`is_drawing` 没清会接着上一笔画，`text_drag.active`
没清会把普通拖动当成拖文字，`_move_initial_state` 没清会拿着一份过期快照去推撤销。

这类 bug 不会当场报错，要等到下一次操作才现形，届时现场已经不在了。所以这里不测
"操作本身对不对"（那是别的文件的事），只测一件事：**转换之后，静止状态是不是真的
静止**。

drag_start_pos 不在核查范围内：它确实会留着上一次按下的位置，但 handle_press 开头
无条件重设它，而读它的 handle_move 只在按住左键时跑——也就是必然在某次 press 之后。
留着不好看，但够不到"下次会走错分支"这条线，所以这里不拿它当失败。
"""
import pytest
from PySide6.QtCore import QEvent, QPointF, QRectF, Qt
from PySide6.QtGui import QColor, QImage, QMouseEvent, QPen
from PySide6.QtWidgets import QApplication

from canvas.items import RectItem, TextItem
from canvas.scene import CanvasScene
from canvas.undo import AddItemCommand
from canvas.view import CanvasView

W, H = 500, 380
RED = QColor("#FF3B30")

# 静止时这些标志各自该是什么。留一个不清，下次鼠标一动就会拐进错误的分支。
RESTING = {
    "is_drawing": False,
    "is_selecting": False,
    "is_dragging_selection": False,
    "text_drag.active": False,
    "text_drag.item": None,
    "text_drag.hover_item": None,
    "item_drag.active": False,
    "pending_text_edit_item": None,
    "controller.is_dragging": False,
    "controller._move_initial_state": None,
    "controller.press_requires_manual_dispatch": False,
}


def _snapshot(view):
    controller = view.smart_edit_controller
    return {
        "is_drawing": view.drawing.active,
        "is_selecting": view.selection_drag.active,
        "is_dragging_selection": view.selection_drag.dragging,
        "text_drag.active": view.text_drag.active,
        "text_drag.item": view.text_drag.item,
        "text_drag.hover_item": view.text_drag.hover_item,
        "item_drag.active": view.item_drag.active,
        "pending_text_edit_item": view.pending_text_edit.item,
        "controller.is_dragging": controller.is_dragging,
        "controller._move_initial_state": controller._move_initial_state,
        "controller.press_requires_manual_dispatch": controller.press_requires_manual_dispatch,
    }


def _assert_at_rest(view, what):
    dirty = {k: v for k, v in _snapshot(view).items() if v != RESTING[k]}
    assert not dirty, f"{what} 之后还留着：{dirty}"


@pytest.fixture
def canvas(qapp):
    created = []

    def make(tool, confirmed=True):
        image = QImage(W, H, QImage.Format.Format_ARGB32_Premultiplied)
        image.fill(QColor("white"))
        scene = CanvasScene(image, QRectF(0, 0, W, H), enable_mosaic=True)
        view = CanvasView(scene)
        created.append((scene, view))
        # 场景不激活，图元的 hasFocus() 永远是 False，"正在编辑文字"那一路走不到
        QApplication.sendEvent(view.viewport(), QEvent(QEvent.Type.WindowActivate))
        if confirmed:
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


def _gesture(view, start, end, steps=4):
    _send(view, QEvent.Type.MouseButtonPress, start)
    for i in range(1, steps + 1):
        _send(view, QEvent.Type.MouseMove, start + (end - start) * (i / steps))
    _send(view, QEvent.Type.MouseButtonRelease, end, Qt.MouseButton.NoButton)


def _put_rect(scene):
    item = RectItem(QRectF(60, 60, 140, 90), QPen(RED, 4))
    scene.addItem(item)
    scene.undo_stack.push(AddItemCommand(scene, item))
    return item


def _write_text(scene, view, text="hello world"):
    _gesture(view, QPointF(120, 120), QPointF(120, 120), steps=1)
    item = next(i for i in scene.items() if isinstance(i, TextItem))
    item.setPlainText(text)
    QApplication.processEvents()
    return item


def _bottom_edge(item):
    rect = item.mapToScene(item.boundingRect()).boundingRect()
    return QPointF(rect.center().x(), rect.bottom() - 2)


# —— 转换 1：画完一笔之后切工具 ——

@pytest.mark.parametrize("tool", ["rect", "ellipse", "arrow", "pen", "mosaic"])
def test_switching_tool_after_drawing_leaves_nothing_behind(canvas, tool):
    scene, view = canvas(tool)
    _gesture(view, QPointF(80, 80), QPointF(220, 180), steps=6)

    scene.tool_controller.activate("number")
    QApplication.processEvents()

    _assert_at_rest(view, f"用 {tool} 画完一笔再切工具")


# —— 转换 2：导出/保存前的清理 ——

def test_editing_cleanup_after_drawing_leaves_nothing_behind(canvas):
    """导出和保存前会发 editing_cleanup_requested，把编辑态收干净再渲染。"""
    scene, view = canvas("pen")
    _gesture(view, QPointF(80, 80), QPointF(220, 180), steps=6)

    scene.editing_cleanup_requested.emit()
    QApplication.processEvents()

    _assert_at_rest(view, "画完一笔后触发 editing_cleanup")


def test_editing_cleanup_after_dragging_an_item_leaves_nothing_behind(canvas):
    scene, view = canvas("rect")
    _put_rect(scene)
    _gesture(view, QPointF(60, 105), QPointF(160, 175), steps=6)

    scene.editing_cleanup_requested.emit()
    QApplication.processEvents()

    _assert_at_rest(view, "拖完图元后触发 editing_cleanup")
    assert view.smart_edit_controller.selected_item is None, "清理该把选中也取消掉"


# —— 转换 3：拖动之后切工具 ——

def test_switching_tool_after_dragging_an_item_leaves_nothing_behind(canvas):
    scene, view = canvas("rect")
    _put_rect(scene)
    _gesture(view, QPointF(60, 105), QPointF(160, 175), steps=6)

    scene.tool_controller.activate("ellipse")
    QApplication.processEvents()

    _assert_at_rest(view, "拖完图元再切工具")
    assert view.smart_edit_controller.selected_item is None, "切工具该清掉选中"


# —— 转换 4：文字编辑 ——

def test_switching_tool_while_editing_text_leaves_nothing_behind(canvas):
    """编辑文字时切到别的工具：编辑要结束，文字要留下。"""
    scene, view = canvas("text")
    item = _write_text(scene, view)
    assert item.is_editing(), "前置条件：正在编辑"

    scene.tool_controller.activate("rect")
    QApplication.processEvents()

    _assert_at_rest(view, "编辑文字时切工具")
    assert item.scene() is not None, "切个工具不该把写好的字弄丢"


def test_switching_tool_after_dragging_text_edge_leaves_nothing_behind(canvas):
    """拖完文字边缘再切工具——这一路的状态归 TextEdgeDrag 管，最容易留尾巴。"""
    scene, view = canvas("text")
    item = _write_text(scene, view)
    edge = _bottom_edge(item)
    _gesture(view, edge, edge + QPointF(50, 35), steps=4)

    scene.tool_controller.activate("rect")
    QApplication.processEvents()

    _assert_at_rest(view, "拖完文字边缘再切工具")


def test_clicking_empty_space_after_editing_text_leaves_nothing_behind(canvas):
    """点空白结束编辑，是最常走的一条退出路径。"""
    scene, view = canvas("text")
    item = _write_text(scene, view)

    _gesture(view, QPointF(420, 330), QPointF(420, 330), steps=1)

    _assert_at_rest(view, "点空白结束文字编辑")
    assert not item.is_editing(), "点了空白还在编辑态"


# —— 转换 5：选区 ——

def test_confirming_a_selection_leaves_nothing_behind(canvas):
    """拉出选区并确认之后，拉选区那套标志要归位。"""
    scene, view = canvas("rect", confirmed=False)

    _gesture(view, QPointF(60, 60), QPointF(300, 260), steps=6)

    assert scene.selection_model.is_confirmed, "前置条件：选区已确认"
    _assert_at_rest(view, "拉出选区并确认")


# —— 转换 6：手势进行中被打断 ——

@pytest.mark.parametrize("interrupt", ["切工具", "editing_cleanup"])
def test_a_gesture_interrupted_midway_still_settles_on_release(canvas, interrupt):
    """拖到一半被打断（快捷键切工具、导出前清理），松手之后必须归位。

    打断发生在手还按着的时候，手势不会当场中止——切工具后接着拖，文字/图元仍跟着
    走，这是对的：一次手势该有始有终。但松手那一下必须把状态收干净，否则下一次
    鼠标移动就会拐进这条已经作废的分支。

    收尾代码在好几条路径上都写了一份（切工具、cleanup、松手、窗口关闭）。这里锁的
    是最后那道：不管前面几道有没有生效，松手之后状态必须是静止的。
    """
    scene, view = canvas("text")
    item = _write_text(scene, view)
    edge = _bottom_edge(item)

    _send(view, QEvent.Type.MouseButtonPress, edge)
    _send(view, QEvent.Type.MouseMove, edge + QPointF(30, 20))
    assert view.text_drag.active, "前置条件：拖动确实开始了"

    if interrupt == "切工具":
        scene.tool_controller.activate("rect")
    else:
        scene.editing_cleanup_requested.emit()
    QApplication.processEvents()

    _send(view, QEvent.Type.MouseMove, edge + QPointF(60, 45))
    _send(view, QEvent.Type.MouseButtonRelease, edge + QPointF(60, 45), Qt.MouseButton.NoButton)

    _assert_at_rest(view, f"拖到一半遇上{interrupt}、随后松手")


# —— 转换 7：来回切换不该积累状态 ——

def test_hopping_between_tools_does_not_accumulate_state(canvas):
    """连续换好几种工具，每种都画一笔——标志不该越攒越多。"""
    scene, view = canvas("rect")
    for tool, start in [
        ("rect", QPointF(60, 60)),
        ("pen", QPointF(90, 90)),
        ("text", QPointF(140, 140)),
        ("arrow", QPointF(180, 180)),
        ("mosaic", QPointF(220, 220)),
    ]:
        scene.tool_controller.activate(tool)
        QApplication.processEvents()
        _gesture(view, start, start + QPointF(60, 50), steps=3)
        if tool == "text":
            # 文字要点开空白才算写完，否则它一直在编辑态
            _gesture(view, QPointF(430, 340), QPointF(430, 340), steps=1)
        _assert_at_rest(view, f"切到 {tool} 画完一笔")
