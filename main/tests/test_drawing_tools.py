# -*- coding: utf-8 -*-
"""
绘图工具行为测试（pen / rect / ellipse / arrow）

现有测试只覆盖了 Tool 基类和序号工具，各个具体绘图工具的
"按下 → 拖动 → 松开" 全流程一直没有测试。

这里用真实的 CanvasScene 和真实的 CommandUndoStack（不 mock），
验证真正要紧的行为：
- 一次完整拖拽后，场景里确实多出一个图元，且几何形状正确
- 拖拽距离过短时按各工具的 MIN_SIZE / MIN_LENGTH 规则被丢弃，不产生垃圾图元
- 绘制会进撤销栈，撤销后图元从场景消失、重做后回来
- 绘制过程中的临时图元不会泄漏在场景里
"""
import pytest
from PySide6.QtWidgets import QApplication
from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import QColor, QImage


@pytest.fixture(scope="module")
def qapp():
    app = QApplication.instance()
    if app is None:
        app = QApplication([])
    yield app


@pytest.fixture
def scene(qapp):
    """真实的 CanvasScene，背景为 400x300 白底"""
    from canvas.scene import CanvasScene
    bg = QImage(400, 300, QImage.Format.Format_ARGB32)
    bg.fill(0xFFFFFFFF)
    return CanvasScene(bg, QRectF(0, 0, 400, 300))


@pytest.fixture
def undo_stack(qapp):
    from canvas.undo import CommandUndoStack
    return CommandUndoStack()


@pytest.fixture
def ctx(scene, undo_stack):
    from tools.base import ToolContext
    from canvas.selection_model import SelectionModel
    return ToolContext(
        scene=scene,
        selection=SelectionModel(),
        undo_stack=undo_stack,
        color=QColor("#FF0000"),
        stroke_width=4,
        opacity=1.0,
    )


def _drawn_items(scene):
    """场景中由工具绘制出来的图元（排除场景自带的背景层与选区层）"""
    from canvas.items import BackgroundItem, SelectionItem
    return [
        it for it in scene.items()
        if not isinstance(it, (BackgroundItem, SelectionItem))
    ]


def _drag(tool, ctx, start, end, steps=3):
    """模拟一次完整拖拽：按下 → 若干次移动 → 松开"""
    tool.on_press(QPointF(*start), Qt.MouseButton.LeftButton, ctx)
    for i in range(1, steps + 1):
        t = i / steps
        tool.on_move(
            QPointF(start[0] + (end[0] - start[0]) * t,
                    start[1] + (end[1] - start[1]) * t),
            ctx,
        )
    tool.on_release(QPointF(*end), ctx)


# ============================================================================
# 画笔
# ============================================================================

class TestPenTool:
    """自由绘制"""

    @pytest.fixture
    def tool(self, qapp):
        from tools.pen import PenTool
        return PenTool()

    def test_drag_creates_one_stroke_item(self, tool, ctx, scene):
        from canvas.items import StrokeItem
        _drag(tool, ctx, (10, 10), (100, 80))

        items = _drawn_items(scene)
        assert len(items) == 1
        assert isinstance(items[0], StrokeItem)

    def test_stroke_follows_the_dragged_path(self, tool, ctx, scene):
        """笔迹的包围盒应覆盖拖拽经过的区域"""
        _drag(tool, ctx, (10, 10), (100, 80))

        rect = _drawn_items(scene)[0].path().boundingRect()
        assert rect.left() == pytest.approx(10, abs=1)
        assert rect.top() == pytest.approx(10, abs=1)
        assert rect.right() == pytest.approx(100, abs=1)
        assert rect.bottom() == pytest.approx(80, abs=1)

    def test_drawing_is_pushed_to_undo_stack(self, tool, ctx, scene, undo_stack):
        _drag(tool, ctx, (10, 10), (100, 80))
        assert undo_stack.canUndo()

        undo_stack.undo()
        assert _drawn_items(scene) == []

        undo_stack.redo()
        assert len(_drawn_items(scene)) == 1

    def test_tool_state_is_reset_after_release(self, tool, ctx):
        """松开后内部状态必须清空，否则下一笔会接着上一笔画"""
        _drag(tool, ctx, (10, 10), (100, 80))
        assert tool.drawing is False
        assert tool.current_item is None
        assert tool.start_pos is None

    def test_two_strokes_are_independent_items(self, tool, ctx, scene):
        _drag(tool, ctx, (10, 10), (50, 50))
        _drag(tool, ctx, (200, 200), (250, 250))

        items = _drawn_items(scene)
        assert len(items) == 2
        assert items[0] is not items[1]

    def test_move_without_press_is_ignored(self, tool, ctx, scene):
        """没有按下就移动（比如鼠标飘过画布）不应产生图元"""
        tool.on_move(QPointF(50, 50), ctx)
        assert _drawn_items(scene) == []

    def test_shift_locks_stroke_to_a_straight_line(self, tool, ctx, scene, monkeypatch):
        """按住 Shift 时应锁定为水平/垂直直线"""
        monkeypatch.setattr(
            QApplication, "keyboardModifiers",
            staticmethod(lambda: Qt.KeyboardModifier.ShiftModifier),
        )
        # 横向位移远大于纵向，应锁成水平线
        _drag(tool, ctx, (10, 50), (200, 62))

        rect = _drawn_items(scene)[0].path().boundingRect()
        assert rect.height() == pytest.approx(0, abs=1e-6)
        assert rect.width() == pytest.approx(190, abs=1)


# ============================================================================
# 矩形
# ============================================================================

class TestRectTool:
    """矩形"""

    @pytest.fixture
    def tool(self, qapp):
        from tools.rect import RectTool
        return RectTool()

    def test_drag_creates_rect_with_expected_geometry(self, tool, ctx, scene):
        from canvas.items import RectItem
        _drag(tool, ctx, (20, 30), (140, 110))

        items = _drawn_items(scene)
        assert len(items) == 1
        assert isinstance(items[0], RectItem)

        rect = items[0].rect()
        assert rect.width() == pytest.approx(120, abs=1)
        assert rect.height() == pytest.approx(80, abs=1)

    def test_dragging_backwards_still_yields_a_normalized_rect(self, tool, ctx, scene):
        """从右下往左上拖，矩形宽高应为正值而不是负值"""
        _drag(tool, ctx, (140, 110), (20, 30))

        rect = _drawn_items(scene)[0].rect()
        assert rect.width() > 0
        assert rect.height() > 0
        assert rect.width() == pytest.approx(120, abs=1)
        assert rect.height() == pytest.approx(80, abs=1)

    def test_tiny_drag_is_discarded(self, tool, ctx, scene, undo_stack):
        """小于 MIN_SIZE 的误触不应留下图元，也不应污染撤销栈"""
        _drag(tool, ctx, (50, 50), (50 + tool.MIN_SIZE - 2, 50 + tool.MIN_SIZE - 2))

        assert _drawn_items(scene) == []
        assert not undo_stack.canUndo()

    def test_undo_removes_the_rect(self, tool, ctx, scene, undo_stack):
        _drag(tool, ctx, (20, 30), (140, 110))
        undo_stack.undo()
        assert _drawn_items(scene) == []


# ============================================================================
# 椭圆
# ============================================================================

class TestEllipseTool:
    """椭圆"""

    @pytest.fixture
    def tool(self, qapp):
        from tools.ellipse import EllipseTool
        return EllipseTool()

    def test_drag_creates_ellipse_with_expected_bounds(self, tool, ctx, scene):
        from canvas.items import EllipseItem
        _drag(tool, ctx, (20, 30), (140, 110))

        items = _drawn_items(scene)
        assert len(items) == 1
        assert isinstance(items[0], EllipseItem)

        rect = items[0].rect()
        assert rect.width() == pytest.approx(120, abs=1)
        assert rect.height() == pytest.approx(80, abs=1)

    def test_tiny_drag_is_discarded(self, tool, ctx, scene, undo_stack):
        _drag(tool, ctx, (50, 50), (52, 52))
        assert _drawn_items(scene) == []
        assert not undo_stack.canUndo()

    def test_undo_redo_round_trip(self, tool, ctx, scene, undo_stack):
        _drag(tool, ctx, (20, 30), (140, 110))
        undo_stack.undo()
        assert _drawn_items(scene) == []
        undo_stack.redo()
        assert len(_drawn_items(scene)) == 1


# ============================================================================
# 箭头
# ============================================================================

class TestArrowTool:
    """箭头"""

    @pytest.fixture
    def tool(self, qapp):
        from tools.arrow import ArrowTool
        return ArrowTool()

    def test_drag_creates_arrow_between_endpoints(self, tool, ctx, scene):
        from canvas.items import ArrowItem
        _drag(tool, ctx, (30, 40), (200, 150))

        items = _drawn_items(scene)
        assert len(items) == 1
        assert isinstance(items[0], ArrowItem)

    def test_short_arrow_is_discarded(self, tool, ctx, scene, undo_stack):
        """短于 MIN_LENGTH 的箭头会退化成一个点，应当丢弃"""
        _drag(tool, ctx, (50, 50), (50 + tool.MIN_LENGTH - 3, 50))

        assert _drawn_items(scene) == []
        assert not undo_stack.canUndo()

    def test_finished_arrow_requests_auto_selection(self, tool, ctx, scene):
        """箭头画完后会发信号请求自动选中，方便接着调整"""
        received = []
        scene.item_auto_select_requested.connect(received.append)

        _drag(tool, ctx, (30, 40), (200, 150))

        assert len(received) == 1
        assert received[0] is _drawn_items(scene)[0]

    def test_undo_removes_the_arrow(self, tool, ctx, scene, undo_stack):
        _drag(tool, ctx, (30, 40), (200, 150))
        undo_stack.undo()
        assert _drawn_items(scene) == []


# ============================================================================
# 跨工具的共同约束
# ============================================================================

class TestNoLeakedPreviewItems:
    """
    绘制过程中工具会先把"预览图元"加进场景，松手时再交给撤销栈接管。
    若某个工具漏掉了移除预览的步骤，场景里就会留下不受撤销栈管理的幽灵图元
    ——撤销之后画面上还残留东西。这里对每个工具统一验证。
    """

    @pytest.mark.parametrize("tool_path,start,end", [
        ("tools.pen:PenTool", (10, 10), (120, 90)),
        ("tools.rect:RectTool", (10, 10), (120, 90)),
        ("tools.ellipse:EllipseTool", (10, 10), (120, 90)),
        ("tools.arrow:ArrowTool", (10, 10), (120, 90)),
    ])
    def test_undo_leaves_scene_clean(self, ctx, scene, undo_stack, tool_path, start, end):
        import importlib
        mod_name, cls_name = tool_path.split(":")
        tool = getattr(importlib.import_module(mod_name), cls_name)()

        _drag(tool, ctx, start, end)
        assert len(_drawn_items(scene)) == 1, f"{cls_name} 未产生图元"

        undo_stack.undo()
        assert _drawn_items(scene) == [], f"{cls_name} 撤销后场景仍有残留图元"
