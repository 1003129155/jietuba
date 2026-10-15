# -*- coding: utf-8 -*-
"""StrokeItem 只重绘变了的、露出来的块：画出来的样子要和整条一次画完的一模一样，屏幕上不留残影。"""
import math

import pytest
from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import QColor, QImage, QLinearGradient, QPainter, QPen, QPixmap
from PySide6.QtWidgets import QApplication, QGraphicsPixmapItem, QGraphicsScene

from canvas.items import NumberItem, StrokeItem
from tools.base import FreehandStroke

W, H = 360, 240


def _pen(kind):
    if kind == "highlighter":
        pen = QPen(QColor(255, 210, 0, 140), 12)
    else:
        pen = QPen(QColor(220, 40, 60, 255 if kind == "solid" else 160), 4)
    pen.setCapStyle(Qt.PenCapStyle.RoundCap)
    pen.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
    if kind == "dashed":
        pen.setStyle(Qt.PenStyle.DashLine)
        pen.setDashPattern([3, 2])
    return pen


def _background():
    image = QImage(W, H, QImage.Format.Format_ARGB32_Premultiplied)
    painter = QPainter(image)
    gradient = QLinearGradient(0, 0, W, H)
    gradient.setColorAt(0, QColor("#2a3d66"))
    gradient.setColorAt(1, QColor("#e8eefc"))
    painter.fillRect(image.rect(), gradient)
    painter.end()
    return image


def _scene():
    """背景用事先画好的图：渐变画刷在局部重绘时会差一个色阶，贴图逐像素一致。"""
    scene = QGraphicsScene(0, 0, W, H)
    scene.addItem(QGraphicsPixmapItem(QPixmap.fromImage(_background())))
    return scene


def _render(scene, source=None):
    source = source or QRectF(0, 0, W, H)
    image = QImage(int(source.width()), int(source.height()), QImage.Format.Format_ARGB32_Premultiplied)
    image.fill(0)
    painter = QPainter(image)
    scene.render(painter, QRectF(0, 0, source.width(), source.height()), source)
    painter.end()
    return image


def _points():
    """慢画的一圈（密、取整）再接几笔快的，路径够长，会分成好几块。"""
    pts = []
    for i in range(260):
        a = i / 260 * 2 * math.pi
        pts.append(QPointF(round(170 + 110 * math.cos(a) + 6 * math.sin(a * 9)),
                           round(120 + 80 * math.sin(a))))
    for i in range(1, 8):
        pts.append(QPointF(170 + 20 * i, 120 + (25 if i % 2 else -25)))
    return pts


def _draw(scene, kind, with_line=False, on_step=None):
    """像画笔工具那样边画边 extend_path，返回笔迹和图元（还没 finish_path）。"""
    pts = _points()
    stroke = FreehandStroke(pts[0], 45, smooth=True)
    item = StrokeItem(stroke.path, _pen(kind), is_highlighter=(kind == "highlighter"))
    scene.addItem(item)
    for k, p in enumerate(pts[1:]):
        straight = with_line and 120 <= k < 140
        target = QPointF(p.x(), p.y() + (k - 120) * 4) if straight else p
        if stroke.extend(target, straight=straight):
            item.extend_path(stroke.path, stroke.take_dirty_from())
            if on_step:
                on_step(item)
    return stroke, item


def _built(scene, kind, path=None):
    """拿整条路径直接新建的笔迹，钉图克隆出来的就是这样。"""
    if path is None:
        path = _draw(QGraphicsScene(), kind)[0].path
    item = StrokeItem(path, _pen(kind), is_highlighter=(kind == "highlighter"))
    scene.addItem(item)
    return item


def _long_path():
    """来回扫几行的长笔迹，分成几十块，各块在画面上离得开。"""
    stroke = FreehandStroke(QPointF(20, 40), 45, smooth=True)
    for i in range(1, 1400):
        row, along = divmod(i * 0.9, 320)
        x = 20 + along if row % 2 == 0 else 340 - along
        stroke.extend(QPointF(x, 40 + 50 * row + 15 * math.sin(i / 7)), False)
    return stroke.path


def _differing(a: QImage, b: QImage):
    return [(x, y) for y in range(a.height()) for x in range(a.width()) if a.pixel(x, y) != b.pixel(x, y)]


@pytest.mark.parametrize("kind", ["solid", "translucent", "highlighter", "dashed"])
def test_drawing_it_bit_by_bit_looks_exactly_like_building_it_in_one_go(qapp, kind):
    drawn_scene, built_scene = _scene(), _scene()
    _draw(drawn_scene, kind)
    _built(built_scene, kind)
    assert _differing(_render(drawn_scene), _render(built_scene)) == []


def test_the_item_keeps_its_own_path_while_drawing(qapp):
    """图元在 extend_path 时只抄画笔改过的那一截，画笔就地改写自己的路径不会直接改到图元。"""
    stroke, item = _draw(_scene(), "solid", with_line=True)
    assert item.path() == stroke.path
    before = item.path()
    stroke.path.setElementPositionAt(stroke.path.elementCount() - 1, 0, 0)
    assert item.path() == before


@pytest.mark.parametrize("kind", ["solid", "translucent", "highlighter"])
def test_solid_strokes_look_exactly_as_qt_draws_the_path(qapp, kind):
    """实线整条露出来时就是一次 drawPath，和换成自定义图元之前画出来的逐像素相同。"""
    scene = _scene()
    item = _built(scene, kind)
    expected = _background()
    painter = QPainter(expected)
    if kind == "highlighter":
        painter.setCompositionMode(QPainter.CompositionMode.CompositionMode_Multiply)
    painter.setRenderHint(QPainter.RenderHint.Antialiasing)
    painter.setPen(item.pen())
    painter.drawPath(item.path())
    painter.end()
    assert _differing(_render(scene), expected) == []


@pytest.mark.parametrize("kind", ["solid", "dashed"])
def test_painting_with_a_bare_style_option_draws_the_whole_stroke(qapp, kind):
    """预览图直接拿新建的 QStyleOptionGraphicsItem 调 paint，重绘区是空的，也要整条画出来。"""
    from PySide6.QtWidgets import QStyleOptionGraphicsItem
    scene = _scene()
    item = _built(scene, kind)
    expected = _render(scene)
    image = _render(_scene())
    painter = QPainter(image)
    item.paint(painter, QStyleOptionGraphicsItem(), None)
    painter.end()
    assert _differing(image, expected) == []


@pytest.mark.parametrize("how", ["drawn", "built"])
@pytest.mark.parametrize("kind", ["solid", "highlighter", "dashed"])
def test_a_partial_repaint_matches_the_full_one(qapp, kind, how):
    """只重绘一块区域时 paint 只画相交的块；这块区域里的像素必须和整幅重绘一致。"""
    scene = _scene()
    if how == "drawn":
        _draw(scene, kind)
    else:
        _built(scene, kind)
    full = _render(scene)
    for rect in (QRectF(250, 90, 40, 40), QRectF(150, 30, 60, 24), QRectF(0, 0, 90, 90), QRectF(57, 171, 31, 17)):
        part = _render(scene, rect)
        crop = full.copy(int(rect.x()), int(rect.y()), int(rect.width()), int(rect.height()))
        assert _differing(part, crop) == [], rect


@pytest.mark.parametrize("kind", ["solid", "dashed"])
def test_a_partial_repaint_draws_only_the_nearby_chunks(qapp, kind):
    """别的图元在笔迹一角重绘时，这条笔迹只连带画那一角的几块，不是整条。"""
    scene = _scene()
    item = _built(scene, kind, _long_path())
    assert len(item._all_chunks()) > 20
    corner = QRectF(150, 80, 20, 20)
    whole = item.boundingRect()
    if kind == "dashed":
        assert item._dash_outline(corner).elementCount() * 8 < item._dash_outline(whole).elementCount()
    else:
        assert item._visible_path(corner).elementCount() * 8 < item.path().elementCount()


@pytest.mark.parametrize("kind", ["solid", "dashed"])
@pytest.mark.parametrize("with_line", [False, True])
def test_marked_rects_cover_every_changed_pixel(qapp, monkeypatch, kind, with_line):
    """每一步标脏的范围要盖住所有真的变了的像素；Shift 直线扫过的旧位置也算。"""
    scene = _scene()
    previous = {"image": _render(scene)}
    missed = []
    calls = []
    original_update = StrokeItem.update

    def recording_update(self, *args):
        calls.append(QRectF(args[0]) if args else self.boundingRect())
        original_update(self, *args)

    def check(_item):
        marked = QRectF()
        for rect in calls:
            marked = marked.united(rect)
        calls.clear()
        current = _render(scene)
        for x, y in _differing(previous["image"], current):
            if not marked.contains(QPointF(x + 0.5, y + 0.5)):
                missed.append((x, y))
        previous["image"] = current

    monkeypatch.setattr(StrokeItem, "update", recording_update)
    _draw(scene, kind, with_line=with_line, on_step=check)
    assert missed == []


def test_bounds_grow_in_big_steps_and_tighten_when_finished(qapp, monkeypatch):
    """外接矩形一变大就要整块重绘一次，所以画的时候每次多扩一截；画完收紧，选中手柄才贴着笔迹。"""
    scene = _scene()
    grows = []
    original = StrokeItem.prepareGeometryChange

    def counting(self):
        grows.append(1)
        original(self)

    monkeypatch.setattr(StrokeItem, "prepareGeometryChange", counting)
    stroke, item = _draw(scene, "solid")
    assert len(grows) <= 3
    for chunk in item._chunks:
        assert item.boundingRect().contains(chunk.rect)

    item.finish_path()
    assert item.boundingRect() == StrokeItem(stroke.path, _pen("solid")).boundingRect()
    assert item.boundingRect().contains(item.shape().boundingRect())


@pytest.mark.parametrize("width,cap", [(1, "round"), (4, "round"), (12, "round"), (40, "round"),
                                       (4, "square"), (12, "square")])
def test_bounds_hold_every_painted_pixel_and_the_whole_hit_area(qapp, width, cap):
    """外接矩形贴着曲线算：画出来的像素露在外面会擦不干净，热区露在外面边上会点不中。"""
    pen = QPen(QColor(220, 40, 60), width)
    if cap == "round":
        pen.setCapStyle(Qt.PenCapStyle.RoundCap)
        pen.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
    item = StrokeItem(_draw(QGraphicsScene(), "solid", with_line=True)[0].path, pen)
    scene = QGraphicsScene()
    scene.addItem(item)
    bounds = item.boundingRect()
    assert bounds.contains(item.shape().boundingRect())

    source = bounds.adjusted(-8, -8, 8, 8)
    image = _render(scene, source)
    outside = [(x, y) for y in range(image.height()) for x in range(image.width())
               if image.pixel(x, y) >> 24 and not bounds.contains(source.topLeft() + QPointF(x + 0.5, y + 0.5))]
    assert outside == []


@pytest.mark.parametrize("kind", ["solid", "dashed"])
def test_restyling_a_finished_stroke_draws_the_new_style(qapp, kind):
    """只换颜色时沿用已切好的块和虚线轮廓，换笔宽要重切；两种都要画得和新建的一样。"""
    scene = _scene()
    stroke, item = _draw(scene, kind)
    item.finish_path()
    _render(scene, QRectF(250, 90, 40, 40))  # 先切好块、算好轮廓

    recolored = item.pen()
    recolored.setColor(QColor(30, 160, 90, 200))
    item.setPen(recolored)
    expected_scene = _scene()
    expected_scene.addItem(StrokeItem(stroke.path, recolored))
    assert _differing(_render(scene), _render(expected_scene)) == []

    item.set_stroke_width(9)
    widened = item.pen()
    expected_scene = _scene()
    expected_scene.addItem(StrokeItem(stroke.path, widened))
    assert _differing(_render(scene), _render(expected_scene)) == []
    assert item.boundingRect() == StrokeItem(stroke.path, widened).boundingRect()


# ============================================================================
# 屏幕上实际留下的像素：视图走自己的局部重绘，grabWindow 读到的是窗口上累积的结果，
# 和整幅重画（view.grab）逐像素比，有一处没擦干净就不相等。
# ============================================================================

class _Screen:
    def __init__(self, scene, scale=1.0, centre=None):
        from PySide6.QtWidgets import QGraphicsView
        self.view = QGraphicsView(scene)
        # 与 CanvasView 相同的更新方式
        self.view.setViewportUpdateMode(QGraphicsView.ViewportUpdateMode.SmartViewportUpdate)
        self.view.setFrameShape(QGraphicsView.Shape.NoFrame)
        self.view.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.view.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.view.resize(W, H)
        self.view.scale(scale, scale)
        if centre is not None:
            self.view.centerOn(centre)
        self.view.show()
        self.settle()

    def settle(self):
        app = QApplication.instance()
        for _ in range(3):
            app.processEvents()

    def stale_pixels(self):
        self.settle()
        fmt = QImage.Format.Format_ARGB32_Premultiplied
        shown = self.view.windowHandle().screen().grabWindow(self.view.winId()).toImage().convertToFormat(fmt)
        fresh = self.view.grab().toImage().convertToFormat(fmt)
        return 0 if shown == fresh else len(_differing(shown, fresh))

    def close(self):
        self.view.close()
        self.view.deleteLater()
        self.settle()


def _canvas_ctx(line_style="solid", angle=45):
    """真实的 CanvasScene（渐变背景，残留的笔迹颜色在哪都看得出来）和工具上下文。"""
    from canvas.scene import CanvasScene
    from canvas.undo import CommandUndoStack
    from canvas.selection_model import SelectionModel
    from tools.base import ToolContext
    scene = CanvasScene(_background().convertToFormat(QImage.Format.Format_ARGB32), QRectF(0, 0, W, H))
    settings = type("Settings", (), {
        "get_shift_line_angle": staticmethod(lambda: angle),
        "get_tool_settings": staticmethod(lambda _tool: {"line_style": line_style}),
        "update_settings": staticmethod(lambda *_args, **_kwargs: None),
    })()
    ctx = ToolContext(scene=scene, selection=SelectionModel(), undo_stack=CommandUndoStack(),
                      color=QColor("#e5484d"), stroke_width=4, opacity=0.8, settings_manager=settings)
    return scene, ctx


def _tool(kind):
    if kind == "highlighter":
        from tools.highlighter import HighlighterTool
        return HighlighterTool()
    from tools.pen import PenTool
    return PenTool()


def _hold(monkeypatch):
    held = [Qt.KeyboardModifier.NoModifier]
    monkeypatch.setattr(QApplication, "keyboardModifiers", staticmethod(lambda: held[0]))
    return held


def _strokes(scene):
    return [item for item in scene.items() if isinstance(item, StrokeItem)]


def _gesture():
    """慢慢画一段，快速大跳几下，按住 Shift 把直线满屏乱甩，松开再画，最后跑出原来的范围。"""
    steps = []
    for i in range(60):
        steps.append((QPointF(40 + i * 1.6, 60 + 14 * math.sin(i / 6)), False))
    for x, y in [(330, 40), (60, 230), (300, 210), (140, 20)]:
        steps.append((QPointF(x, y), False))
    for x, y in [(355, 10), (5, 235), (355, 235), (5, 5), (180, 238), (358, 120), (150, 130)]:
        steps.append((QPointF(x, y), True))
    for i in range(40):
        steps.append((QPointF(150 + i * 5, 130 + 40 * math.sin(i / 4)), False))
    for x, y in [(-60, -40), (420, 300), (200, 120)]:
        steps.append((QPointF(x, y), False))
    return steps


@pytest.mark.parametrize("kind,line_style,scale", [
    ("pen", "solid", 1.0),
    ("pen", "dashed", 1.0),
    ("highlighter", "solid", 1.0),
    ("pen", "solid", 2.5),   # 钉图放大
    ("pen", "dashed", 2.5),
    ("pen", "solid", 0.5),   # 钉图缩小
])
def test_no_stale_pixels_while_drawing_and_after_release(qapp, monkeypatch, kind, line_style, scale):
    scene, ctx = _canvas_ctx(line_style)
    screen = _Screen(scene, scale, QPointF(180, 120))
    tool = _tool(kind)
    held = _hold(monkeypatch)
    stale = []
    try:
        tool.on_press(QPointF(40, 60), Qt.MouseButton.LeftButton, ctx)
        for k, (pos, straight) in enumerate(_gesture()):
            held[0] = Qt.KeyboardModifier.ShiftModifier if straight else Qt.KeyboardModifier.NoModifier
            tool.on_move(pos, ctx)
            if screen.stale_pixels():
                stale.append(k)
        tool.on_release(QPointF(200, 120), ctx)
        [item] = _strokes(scene)
        assert ctx.undo_stack.count() == 1 and ctx.undo_stack.command(0).item is item
        assert item.boundingRect() == StrokeItem(item.path(), item.pen()).boundingRect()
        assert screen.stale_pixels() == 0, "松手收紧外接矩形后留下了残影"
    finally:
        screen.close()
    assert stale == [], f"这些步骤后屏幕上有没擦掉的像素: {stale}"


@pytest.mark.parametrize("line_style", ["solid", "dashed"])
def test_other_items_repainting_over_a_finished_stroke_leave_nothing_behind(qapp, monkeypatch, line_style):
    """别的图元从笔迹上拖过去，笔迹只局部重绘；虚线的节奏也要和整条画出来的接得上。"""
    scene, ctx = _canvas_ctx(line_style)
    screen = _Screen(scene)
    tool = _tool("pen")
    _hold(monkeypatch)
    try:
        tool.on_press(QPointF(40, 60), Qt.MouseButton.LeftButton, ctx)
        for pos, _straight in _gesture():
            tool.on_move(pos, ctx)
        tool.on_release(QPointF(200, 120), ctx)
        [item] = _strokes(scene)
        for rect in (QRectF(100, 60, 30, 30), QRectF(200, 100, 17, 41), QRectF(40, 150, 60, 9)):
            item.update(rect)
            assert screen.stale_pixels() == 0, rect
        marker = NumberItem(1, QPointF(20, 20), 12, QColor("#3b82f6"))
        scene.addItem(marker)
        for i in range(30):
            marker.setPos(20 + i * 11, 30 + 6 * i)
            assert screen.stale_pixels() == 0, i
    finally:
        screen.close()


def test_moving_a_finished_stroke_leaves_nothing_behind(qapp, monkeypatch):
    scene, ctx = _canvas_ctx()
    screen = _Screen(scene)
    tool = _tool("pen")
    _hold(monkeypatch)
    try:
        tool.on_press(QPointF(40, 60), Qt.MouseButton.LeftButton, ctx)
        for pos, _straight in _gesture()[:70]:
            tool.on_move(pos, ctx)
        tool.on_release(QPointF(140, 20), ctx)
        [item] = _strokes(scene)
        for dx, dy in [(30, 10), (90, -20), (-40, 60), (150, 100), (0, 0)]:
            item.setPos(dx, dy)
            assert screen.stale_pixels() == 0, (dx, dy)
        item.set_stroke_width(1)
        assert screen.stale_pixels() == 0, "笔变细后外接矩形缩小，原来的边缘要擦掉"
    finally:
        screen.close()


@pytest.mark.parametrize("kind", ["pen", "highlighter"])
def test_switching_tools_mid_stroke_keeps_the_stroke(qapp, monkeypatch, kind):
    scene, ctx = _canvas_ctx()
    screen = _Screen(scene)
    tool = _tool(kind)
    _hold(monkeypatch)
    try:
        tool.on_press(QPointF(40, 60), Qt.MouseButton.LeftButton, ctx)
        for pos, _straight in _gesture()[:70]:
            tool.on_move(pos, ctx)
        tool.on_deactivate(ctx)
        assert len(_strokes(scene)) == 1
        assert ctx.undo_stack.count() == 1
        # 切走后才来的松手不归它管，也不能再动那一笔
        tool.on_release(QPointF(300, 200), ctx)
        assert ctx.undo_stack.count() == 1
        assert screen.stale_pixels() == 0
        ctx.undo_stack.undo()
        assert _strokes(scene) == []
        assert screen.stale_pixels() == 0
    finally:
        screen.close()


@pytest.mark.parametrize("kind", ["pen", "highlighter"])
def test_a_press_without_the_previous_release_keeps_both_strokes(qapp, monkeypatch, kind):
    """按着鼠标时焦点被抢走，松手事件没送到；下一次按下时上一笔照松手收下。"""
    scene, ctx = _canvas_ctx()
    screen = _Screen(scene)
    tool = _tool(kind)
    _hold(monkeypatch)
    try:
        tool.on_press(QPointF(40, 60), Qt.MouseButton.LeftButton, ctx)
        for pos, _straight in _gesture()[:40]:
            tool.on_move(pos, ctx)
        tool.on_press(QPointF(200, 200), Qt.MouseButton.LeftButton, ctx)
        for i in range(1, 30):
            tool.on_move(QPointF(200 + i * 4, 200 - i * 2), ctx)
        tool.on_release(QPointF(316, 142), ctx)
        assert len(_strokes(scene)) == 2
        assert ctx.undo_stack.count() == 2
        assert screen.stale_pixels() == 0
    finally:
        screen.close()


@pytest.mark.parametrize("kind,line_style", [("pen", "solid"), ("pen", "dashed"), ("highlighter", "solid")])
def test_a_click_without_moving_leaves_a_dot(qapp, monkeypatch, kind, line_style):
    """按下就松开（手抖不到加点的距离）画成按下处一个笔宽的圆点，能点选，占一步撤销。"""
    scene, ctx = _canvas_ctx(line_style)
    tool = _tool(kind)
    _hold(monkeypatch)
    before = _render(scene)
    tool.on_press(QPointF(100, 100), Qt.MouseButton.LeftButton, ctx)
    tool.on_move(QPointF(100.4, 100.3), ctx)
    tool.on_release(QPointF(100.4, 100.3), ctx)
    [item] = _strokes(scene)
    assert ctx.undo_stack.count() == 1
    assert item.contains(QPointF(100, 100))

    inked = _differing(before, _render(scene))
    radius = item.pen().widthF() / 2
    assert len(inked) >= math.pi * radius * radius
    assert all(math.hypot(x + 0.5 - 100, y + 0.5 - 100) <= radius + 1 for x, y in inked)
    xs, ys = [x for x, _ in inked], [y for _, y in inked]
    assert max(xs) - min(xs) + 1 >= 2 * radius and max(ys) - min(ys) + 1 >= 2 * radius
