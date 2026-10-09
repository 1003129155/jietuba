"""编辑中的文字：悬停显示四向箭头的地方，按下去就该拖动整段，而不是选字。

悬停换光标由 TextItem 自己判，按下要不要拖由 View 的 TextEdgeDrag 判。两处各算
一份边缘就会对不上：看着能拖，按下去却落进 Qt 原生的拖选文字。这里在框和字之间
的那圈空当上逐处核对两件事是否一致。
"""
import pytest
from PySide6.QtCore import QEvent, QPointF, QRectF, Qt
from PySide6.QtGui import QColor, QFont, QImage, QMouseEvent
from PySide6.QtWidgets import QApplication, QGraphicsTextItem

from canvas.items import TextItem
from canvas.scene import CanvasScene
from canvas.view import CanvasView

W, H = 400, 300


@pytest.fixture
def canvas(qapp):
    created = []

    def make():
        image = QImage(W, H, QImage.Format.Format_ARGB32_Premultiplied)
        image.fill(QColor("white"))
        scene = CanvasScene(image, QRectF(0, 0, W, H), enable_mosaic=True)
        view = CanvasView(scene)
        created.append((scene, view))
        # 场景不激活，图元的 hasFocus() 永远是 False，"正在编辑文字"这一路走不到
        QApplication.sendEvent(view.viewport(), QEvent(QEvent.Type.WindowActivate))
        scene.selection_model.initialize_confirmed_rect(QRectF(0, 0, W, H))
        scene.tool_controller.activate("text")
        return scene, view

    yield make

    for scene, view in created:
        view.close()
        scene.deleteLater()


def _send(view, kind, scene_pos, buttons=Qt.MouseButton.LeftButton):
    view_pos = QPointF(view.mapFromScene(scene_pos))
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


def _editing_text(scene, view, *, effects=False, rotation=0.0):
    item = TextItem("hello world", QPointF(120, 110), QFont("Arial", 28), QColor("black"))
    if effects:
        item.set_outline(True, width=max(TextItem.OUTLINE_WIDTH_LEVELS))
        item.set_shadow(True)
    item.setTransformOriginPoint(item.interaction_rect().center())
    item.setRotation(rotation)
    scene.addItem(item)
    view._enter_text_edit_mode(item)
    QApplication.processEvents()
    assert item.is_editing(), "前置条件：文字此刻在编辑态"
    return item


def _text_area(item) -> QRectF:
    """字排在哪儿：文档区域去掉文档边距。"""
    margin = item.document().documentMargin()
    return QGraphicsTextItem.boundingRect(item).adjusted(margin, margin, -margin, -margin)


# 框和字之间那圈空当上的点（图元局部坐标）。四角按钮的命中区往里伸 15px，字号
# 取大一些，左右空当的中点才不会落进去。
GAP_POINTS = {
    "左侧空当": lambda text: QPointF(text.left() - 1.5, text.center().y()),
    "右侧空当": lambda text: QPointF(text.right() + 1.5, text.center().y()),
    "上方边距": lambda text: QPointF(text.center().x(), text.top() - 1.5),
    "下方边距": lambda text: QPointF(text.center().x(), text.bottom() + 1.5),
}


@pytest.mark.parametrize(
    "where, effects, rotation",
    [(name, False, 0.0) for name in GAP_POINTS]
    + [("下方边距", True, 0.0), ("左侧空当", False, 30.0)],
)
def test_pressing_where_the_drag_cursor_shows_drags_the_text(canvas, where, effects, rotation):
    scene, view = canvas()
    item = _editing_text(scene, view, effects=effects, rotation=rotation)
    local = GAP_POINTS[where](_text_area(item))
    scene_pos = item.mapToScene(local)

    assert view.smart_edit_controller.layer_editor.hit_test(scene_pos) is None, "前置条件：没点在按钮上"
    item._apply_edit_cursor(local)
    assert item.cursor().shape() == Qt.CursorShape.SizeAllCursor, "前置条件：这里悬停显示可拖"

    start = QPointF(item.pos())
    delta = QPointF(40, 30)
    _send(view, QEvent.Type.MouseButtonPress, scene_pos)
    assert view.text_drag.active, f"{where}：显示可拖，按下去却没开始拖动"
    _send(view, QEvent.Type.MouseMove, scene_pos + delta)
    _send(view, QEvent.Type.MouseButtonRelease, scene_pos + delta, Qt.MouseButton.NoButton)

    assert QPointF(item.pos()) == start + delta, f"{where}：文字没跟着拖走"
    assert not item.textCursor().hasSelection(), f"{where}：拖动变成了选字"


def test_pressing_on_the_text_itself_still_places_the_caret(canvas):
    """字上面仍归输入光标：按下不拖，悬停是工字光标。"""
    scene, view = canvas()
    item = _editing_text(scene, view)
    local = _text_area(item).center()

    item._apply_edit_cursor(local)
    assert item.cursor().shape() == Qt.CursorShape.IBeamCursor

    _send(view, QEvent.Type.MouseButtonPress, item.mapToScene(local))
    assert not view.text_drag.active
    _send(view, QEvent.Type.MouseButtonRelease, item.mapToScene(local), Qt.MouseButton.NoButton)
    assert item.is_editing()
