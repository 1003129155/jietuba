# -*- coding: utf-8 -*-
"""
手柄浮层的失效不变量测试。

控制点不是 QGraphicsItem，不参与 Qt 的失效核算。历史上它们画在场景的
drawForeground 里，靠调用方手写一个外扩过的脏区（margin = 25）兜住——猜错
就是残影，而且每加一种手柄都得重新猜。

改成独立浮层之后，这里锁住三条不变量：

1. 浮层始终铺满整个 viewport。它的几何不能跟着手柄走：那样几何就成了
   (手柄 × viewportTransform × viewport 尺寸) 的缓存投影，滚轮缩放这种
   没通知到的变换会让它卡在旧矩形上，直接把手柄裁没。
2. 失效区域必须盖住手柄的新位置和旧位置，否则旧位置的像素擦不掉 —— 残影。
3. 失效区域必须远小于整个 viewport，否则半透明浮层会连带把内容层整块重绘，
   比它要取代的整场景重绘还贵。
"""
import math

import pytest
from PySide6.QtCore import QPointF, QRect, QRectF
from PySide6.QtGui import QColor, QFont, QImage, QPainter, QPen
from PySide6.QtWidgets import QApplication


@pytest.fixture(scope="module")
def qapp():
    app = QApplication.instance()
    if app is None:
        app = QApplication([])
    yield app


@pytest.fixture
def view(qapp):
    from canvas.scene import CanvasScene
    from canvas.view import CanvasView

    bg = QImage(800, 600, QImage.Format.Format_ARGB32)
    bg.fill(0xFFFFFFFF)
    v = CanvasView(CanvasScene(bg, QRectF(0, 0, 800, 600)))
    v.resize(800, 600)
    v.show()
    qapp.processEvents()
    yield v
    v.cleanup()


def _select(view, item):
    view.canvas_scene.addItem(item)
    view.smart_edit_controller.select_item(item)
    return item


def _visible_handle_rects(view):
    """每个手柄在 viewport 坐标下、落在视口内的绘制区域。"""
    editor = view.smart_edit_controller.layer_editor
    transform = view.viewportTransform()
    viewport_rect = view.viewport().rect()
    rects = []
    for handle in editor.handles:
        painted = transform.mapRect(handle.get_rect()).toAlignedRect()
        visible = painted.intersected(viewport_rect)
        if not visible.isEmpty():
            rects.append((handle.handle_type, visible))
    return rects


def _assert_painted_covers_handles(view, qapp):
    """跑完一轮事件循环后，浮层实际画过的区域必须盖住每个可见手柄。"""
    qapp.processEvents()
    editor = view.smart_edit_controller.layer_editor
    assert editor.is_editing(), "前置条件：应处于编辑态"

    rects = _visible_handle_rects(view)
    assert rects, "前置条件：至少要有一个手柄落在视口内"

    painted = view._handle_overlay._painted
    for handle_type, rect in rects:
        assert painted.contains(rect), (
            f"手柄 {handle_type} 的绘制区域 {rect} 不在浮层画过的区域 {painted} 内"
        )


class _UpdateSpy:
    """记录浮层被要求失效的所有矩形。"""

    def __init__(self, overlay):
        self.overlay = overlay
        self.rects = []
        self._real = overlay.update

    def __enter__(self):
        def spy(*args):
            if args and isinstance(args[0], QRect):
                self.rects.append(QRect(args[0]))
            else:
                self.rects.append(self.overlay.rect())
            return self._real(*args)

        self.overlay.update = spy
        return self

    def __exit__(self, *exc):
        del self.overlay.update

    def union(self):
        total = QRect()
        for r in self.rects:
            total = total.united(r)
        return total


# ---------------------------------------------------------------------------
# 不变量 1：浮层铺满 viewport，几何不会过期
# ---------------------------------------------------------------------------

def test_overlay_spans_the_whole_viewport(view, qapp):
    from canvas.items import RectItem

    _select(view, RectItem(QRectF(300, 250, 120, 90), QPen(QColor("red"), 3)))
    qapp.processEvents()
    assert view._handle_overlay.geometry() == view.viewport().rect()

    view.resize(600, 400)
    qapp.processEvents()
    assert view._handle_overlay.geometry() == view.viewport().rect()


def test_zoom_needs_no_notification(view, qapp):
    """缩放会整块重绘 viewport，浮层被一并重绘——不该依赖任何人来通知它。

    浮层几何若跟着手柄走，这里就会卡在旧矩形上把手柄裁掉。
    """
    from canvas.items import RectItem

    _select(view, RectItem(QRectF(300, 250, 120, 90), QPen(QColor("red"), 3)))
    _assert_painted_covers_handles(view, qapp)

    view.scale(2.0, 2.0)           # 只改变换，不调 request_handles_repaint
    _assert_painted_covers_handles(view, qapp)

    view.resetTransform()
    _assert_painted_covers_handles(view, qapp)


# ---------------------------------------------------------------------------
# 不变量 2：失效区域盖住新旧两处
# ---------------------------------------------------------------------------

def test_rect_handles_are_painted(view, qapp):
    from canvas.items import RectItem

    _select(view, RectItem(QRectF(300, 250, 200, 150), QPen(QColor("red"), 3)))
    _assert_painted_covers_handles(view, qapp)


@pytest.mark.parametrize("angle", [0, 15, 45, 90, 137, 270])
def test_rotated_text_handles_are_painted(view, qapp, angle):
    """旋转会把包围盒撑大，手柄跟着往外跑——最容易漏的场景。"""
    from canvas.items import TextItem

    item = TextItem("标注 Abc", QPointF(320, 260), QFont("Arial", 20), QColor("red"))
    _select(view, item)
    item.setRotation(angle)
    view.request_handles_repaint()
    _assert_painted_covers_handles(view, qapp)


def test_arrow_control_point_handle_is_painted(view, qapp):
    """箭头的弯曲控制点可以离图元包围盒很远。"""
    from canvas.items import ArrowItem

    item = ArrowItem(QPointF(120, 120), QPointF(500, 160), QPen(QColor("red"), 6))
    _select(view, item)
    item.set_control_point(QPointF(300, 520))
    view.request_handles_repaint()
    _assert_painted_covers_handles(view, qapp)


def test_number_handles_are_painted(view, qapp):
    """序号的加减删按钮排在包围盒外侧，还要罩住那圈虚线框。"""
    from canvas.items import NumberItem

    _select(view, NumberItem(7, QPointF(300, 300), 18.0, QColor("red")))
    _assert_painted_covers_handles(view, qapp)


def test_move_invalidates_both_old_and_new_positions(view, qapp):
    """图元挪走之后，旧位置的手柄像素必须被失效掉，否则就是残影。"""
    from canvas.items import RectItem

    item = _select(view, RectItem(QRectF(150, 120, 150, 100), QPen(QColor("red"), 3)))
    qapp.processEvents()
    before = QRect(view._handle_overlay._painted)
    assert not before.isEmpty()

    item.moveBy(300, 260)
    with _UpdateSpy(view._handle_overlay) as spy:
        view._update_edit_handles()
    dirty = spy.union()

    assert dirty.contains(before), f"失效区域 {dirty} 没盖住旧位置 {before}"
    _assert_painted_covers_handles(view, qapp)
    assert dirty.contains(view._handle_overlay._painted), "失效区域没盖住新位置"


@pytest.mark.parametrize("angle", [15, 45, 90, 137])
def test_rotated_text_move_invalidates_old_position(view, qapp, angle):
    """旋转过的文字被拖动时，旧位置的手柄像素必须被失效掉。

    上面那条 move 用的是未旋转的 RectItem。旋转分支要单独锁：setRotation 之后
    轴对齐包围盒被撑大，手柄锚点落在包围盒的角上，离图元实际画到的像素很远，
    是"失效区域算漏"最可能出现的地方。
    """
    from canvas.items import TextItem

    item = TextItem("标注 Abcdef", QPointF(320, 260), QFont("Arial", 22), QColor("red"))
    _select(view, item)
    item.setRotation(angle)
    view.request_handles_repaint()
    qapp.processEvents()

    before = QRect(view._handle_overlay._painted)
    assert not before.isEmpty(), "前置条件：旋转后应已画出手柄"

    item.moveBy(180, 150)
    with _UpdateSpy(view._handle_overlay) as spy:
        view._update_edit_handles()
    dirty = spy.union()

    assert dirty.contains(before), f"失效区域 {dirty} 没盖住旧位置 {before}"
    _assert_painted_covers_handles(view, qapp)


@pytest.mark.parametrize("angle", [15, 45, 137])
def test_rotated_text_move_frame_by_frame(view, qapp, angle):
    """逐帧模拟拖动旋转过的文字，复刻 view.mouseMoveEvent 里的那两行：
    先 moveBy，再 _update_edit_handles。每一帧都要盖住上一帧画过的地方。
    """
    from canvas.items import TextItem

    item = TextItem("标注 Abcdef", QPointF(200, 180), QFont("Arial", 22), QColor("red"))
    _select(view, item)
    item.setRotation(angle)
    view.request_handles_repaint()
    qapp.processEvents()

    for step in range(1, 9):
        previous = QRect(view._handle_overlay._painted)
        assert not previous.isEmpty(), f"第 {step} 帧前置条件：上一帧应画过手柄"

        item.moveBy(26, 21)
        with _UpdateSpy(view._handle_overlay) as spy:
            view._update_edit_handles()
        dirty = spy.union()

        assert dirty.contains(previous), (
            f"第 {step} 帧（角度 {angle}）失效区域 {dirty} 没盖住上一帧 {previous}"
        )
        qapp.processEvents()


# ---------------------------------------------------------------------------
# 不变量 4：锚点跟住图元的旋转，而不是轴对齐包围盒的角
# ---------------------------------------------------------------------------

def _assert_anchored(editor, item, expected):
    """expected: {HandleType: 期望的 scene 锚点}。"""
    editor.handles = editor._generate_handles(item)
    seen = set()
    for handle in editor.handles:
        want = expected.get(handle.handle_type)
        if want is None:
            continue
        seen.add(handle.handle_type)
        offset = math.hypot(
            handle.position.x() - want.x(), handle.position.y() - want.y()
        )
        assert offset < 0.5, (
            f"{handle.handle_type} 锚点 {handle.position} 偏离旋转后的角 {want}"
            f"（差 {offset:.2f}px）"
        )
    assert seen == set(expected), f"缺少手柄：{set(expected) - seen}"


@pytest.mark.parametrize("angle", [0, 15, 45, 90, 137, 270])
def test_text_handles_anchor_to_rotated_corners(view, qapp, angle):
    """旋转后手柄必须落在文字真正的角上。

    取 sceneBoundingRect() 的角会甩出去（实测 45° 偏 35px，137° 偏 239px）：
    那样不只画错位置，也点不到——hit_test 用的是同一个坐标。
    """
    from canvas.handle_editor import HandleType
    from canvas.items import TextItem

    item = TextItem("标注 Abcdef", QPointF(320, 260), QFont("Arial", 22), QColor("red"))
    _select(view, item)
    item.setRotation(angle)
    view.request_handles_repaint()
    qapp.processEvents()

    local = item.boundingRect()
    _assert_anchored(
        view.smart_edit_controller.layer_editor,
        item,
        {
            HandleType.ROTATE: item.mapToScene(local.topLeft()),
            HandleType.ITEM_DELETE: item.mapToScene(local.topRight()),
            HandleType.TEXT_SCALE: item.mapToScene(local.bottomRight()),
        },
    )


@pytest.mark.parametrize("angle", [0, 30, 45, 137])
def test_rect_handles_anchor_to_rotated_corners(view, qapp, angle):
    """8 点手柄同样要跟住旋转——矩形/椭圆走的是通用锚点路径。"""
    from canvas.handle_editor import HandleType
    from canvas.items import RectItem

    item = _select(view, RectItem(QRectF(300, 250, 200, 120), QPen(QColor("red"), 3)))
    item.setRotation(angle)
    view.request_handles_repaint()
    qapp.processEvents()

    local = item.rect()
    center = local.center()
    _assert_anchored(
        view.smart_edit_controller.layer_editor,
        item,
        {
            HandleType.ROTATE: item.mapToScene(local.topLeft()),
            HandleType.CORNER_TR: item.mapToScene(local.topRight()),
            HandleType.CORNER_BR: item.mapToScene(local.bottomRight()),
            HandleType.CORNER_BL: item.mapToScene(local.bottomLeft()),
            HandleType.EDGE_T: item.mapToScene(QPointF(center.x(), local.top())),
            HandleType.EDGE_R: item.mapToScene(QPointF(local.right(), center.y())),
            HandleType.EDGE_B: item.mapToScene(QPointF(center.x(), local.bottom())),
            HandleType.EDGE_L: item.mapToScene(QPointF(local.left(), center.y())),
        },
    )


@pytest.mark.parametrize("angle", [0, 35, 137])
def test_move_without_any_notification_still_invalidates(view, qapp, angle):
    """图元被挪走而调用方**什么都没通知**时，旧位置仍然必须被失效掉。

    这是对抗样本，不是理论情况：编辑态的文字被 Qt 原生拖走时
    （_handle_selected_item_drag 里"文字编辑中拖拽=选文字"那条分支把手势交给
    super().mouseMoveEvent 后直接 return），既没刷浮层也没更新场景，于是每一帧
    的手柄都留在原地，拖出一串残影。真实屏幕抓图实测：残留主题色像素从 55 降到 0。

    所以这里刻意不调用 _update_edit_handles / request_handles_repaint —— 手柄
    跟不跟得上，必须由浮层自己对场景变化的订阅来保证。
    """
    from canvas.items import TextItem

    item = TextItem("1212", QPointF(300, 240), QFont("Arial", 24), QColor("yellow"))
    _select(view, item)
    item.setRotation(angle)
    view.request_handles_repaint()
    qapp.processEvents()

    before = QRect(view._handle_overlay._painted)
    assert not before.isEmpty(), "前置条件：应已画出手柄"

    with _UpdateSpy(view._handle_overlay) as spy:
        item.moveBy(170, 140)          # 唯一的动作：挪走
        qapp.processEvents()
    dirty = spy.union()

    assert not dirty.isEmpty(), (
        "图元挪走后浮层完全没被失效——没人通知它，它也没订阅场景变化"
    )
    assert dirty.contains(before), f"失效区域 {dirty} 没盖住旧位置 {before}"
    _assert_painted_covers_handles(view, qapp)


@pytest.mark.parametrize("angle", [45, 137])
def test_rotated_text_rotate_handle_is_clickable_at_its_corner(view, qapp, angle):
    """点在文字旋转后的左上角，必须命中旋转手柄。

    锚点错位时这里会失败：手柄画在包围盒角上，用户得点到离文字上百像素远
    的空白处才抓得到它。
    """
    from canvas.handle_editor import HandleType
    from canvas.items import TextItem

    item = TextItem("标注 Abcdef", QPointF(320, 260), QFont("Arial", 22), QColor("red"))
    _select(view, item)
    item.setRotation(angle)
    view.request_handles_repaint()
    qapp.processEvents()

    editor = view.smart_edit_controller.layer_editor
    editor.handles = editor._generate_handles(item)
    corner = item.mapToScene(item.boundingRect().topLeft())

    hit = editor.hit_test(QPointF(corner))
    assert hit is not None, f"角度 {angle}：在文字旋转后的左上角没命中任何手柄"
    assert hit.handle_type == HandleType.ROTATE, (
        f"角度 {angle}：命中的是 {hit.handle_type}，不是旋转手柄"
    )


def test_rotation_drag_invalidates_every_frame(view, qapp):
    """逐帧旋转：每一帧的失效区域都要盖住上一帧画过的地方。"""
    from canvas.handle_editor import HandleType
    from canvas.items import TextItem
    from canvas.smart_edit_controller import SelectionMode

    item = TextItem("标注 Abcdef", QPointF(320, 260), QFont("Arial", 22), QColor("red"))
    _select(view, item)
    qapp.processEvents()

    editor = view.smart_edit_controller.layer_editor
    rotate = next(h for h in editor.handles if h.handle_type == HandleType.ROTATE)
    editor.start_drag(rotate, rotate.position)
    view.smart_edit_controller.mode = SelectionMode.DRAGGING_HANDLE

    for step in range(1, 9):
        previous = QRect(view._handle_overlay._painted)
        with _UpdateSpy(view._handle_overlay) as spy:
            view.smart_edit_controller.handle_edit_move(
                QPointF(360 + step * 28, 210 + step * 22)
            )
        dirty = spy.union()
        assert dirty.contains(previous), (
            f"第 {step} 帧的失效区域 {dirty} 没盖住上一帧画过的 {previous}，会留拖影"
        )
        _assert_painted_covers_handles(view, qapp)


def test_clearing_selection_erases_the_last_handles(view, qapp):
    from canvas.items import RectItem

    _select(view, RectItem(QRectF(300, 250, 150, 100), QPen(QColor("red"), 3)))
    qapp.processEvents()
    before = QRect(view._handle_overlay._painted)
    assert not before.isEmpty()

    with _UpdateSpy(view._handle_overlay) as spy:
        view.smart_edit_controller.clear_selection()
    assert spy.union().contains(before), "取消选中必须失效掉最后画过的手柄区域"

    qapp.processEvents()
    assert view._handle_overlay._painted.isEmpty()


def test_hover_change_requests_a_repaint(view, qapp):
    """悬停高亮参与绘制，hover 变化必须触发重绘。

    改造前 update_hover() 只赋值不失效，高亮全靠别的东西恰好弄脏那块区域。
    """
    from canvas.items import RectItem

    _select(view, RectItem(QRectF(300, 250, 200, 150), QPen(QColor("red"), 3)))
    editor = view.smart_edit_controller.layer_editor

    calls = []
    editor.repaint_requested = lambda: calls.append(1)

    handle_pos = editor.handles[0].position
    assert editor.update_hover(handle_pos) is True
    assert calls, "悬停到手柄上应请求重绘"

    before = len(calls)
    assert editor.update_hover(handle_pos) is False
    assert len(calls) == before, "悬停目标没变时不应重复请求重绘"

    assert editor.update_hover(QPointF(700, 560)) is True
    assert len(calls) > before, "移出手柄应请求重绘以清掉高亮"


# ---------------------------------------------------------------------------
# 不变量 3：失效面积远小于整个 viewport
# ---------------------------------------------------------------------------

def test_invalidated_area_is_far_smaller_than_the_viewport(view, qapp):
    """整层失效会连带把内容层全部重绘，比它要取代的整场景重绘还贵。"""
    from canvas.items import RectItem

    item = _select(view, RectItem(QRectF(300, 250, 150, 100), QPen(QColor("red"), 3)))
    qapp.processEvents()

    item.moveBy(12, 9)
    with _UpdateSpy(view._handle_overlay) as spy:
        view._update_edit_handles()
    dirty = spy.union()

    viewport = view.viewport().rect()
    assert dirty.width() * dirty.height() < viewport.width() * viewport.height() / 4


def test_export_does_not_bake_handles_into_the_image(view, qapp):
    """控制点不在场景里，scene.render() 导出时就不可能被烤进图片。

    改造前它们画在 scene.drawForeground，而 export 只隐藏了选区框、
    没管控制点。
    """
    from canvas.items import RectItem

    item = RectItem(QRectF(100, 100, 200, 150), QPen(QColor("red"), 3))
    view.canvas_scene.addItem(item)

    def _render():
        img = QImage(400, 400, QImage.Format.Format_ARGB32)
        img.fill(0)
        painter = QPainter(img)
        try:
            view.canvas_scene.render(painter, QRectF(0, 0, 400, 400), QRectF(0, 0, 400, 400))
        finally:
            painter.end()
        return img

    without_selection = _render()
    view.smart_edit_controller.select_item(item)
    assert view.smart_edit_controller.layer_editor.is_editing()
    with_selection = _render()

    assert with_selection == without_selection, "导出结果不应因为选中态而变化"
