# -*- coding: utf-8 -*-
"""
CanvasView 属性应用逻辑测试

view.py 有 1500 多行，绝大部分是鼠标/键盘事件处理，难以脱离真实交互测试。
但其中"把工具栏上的调整应用到当前选中图元"这条链路是可以单测的，
而且它恰恰是最容易出错的地方：粗细、透明度、线型三个入口各自要处理
文字图元、荧光笔、未选中等分支。

这里用真实的 CanvasView + CanvasScene（offscreen 渲染），
把 smart_edit_controller 的选中状态直接置位，验证属性变更的落点与边界条件。
"""
import pytest
from PySide6.QtWidgets import QApplication
from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import QColor, QPen, QPainterPath, QFont, QImage


@pytest.fixture(scope="module")
def qapp():
    app = QApplication.instance()
    if app is None:
        app = QApplication([])
    yield app


@pytest.fixture
def view(qapp):
    """真实的 CanvasView（离屏），用完主动 cleanup 避免信号悬挂"""
    from canvas.scene import CanvasScene
    from canvas.view import CanvasView
    bg = QImage(400, 300, QImage.Format.Format_ARGB32)
    bg.fill(0xFFFFFFFF)
    scene = CanvasScene(bg, QRectF(0, 0, 400, 300))
    v = CanvasView(scene)
    yield v
    v.cleanup()


def _select(view, item):
    """把图元放进场景并置为当前选中项"""
    view.canvas_scene.addItem(item)
    view.smart_edit_controller.selected_item = item
    return item


def _stroke(width=4.0, highlighter=False):
    from canvas.items import StrokeItem
    path = QPainterPath(QPointF(0, 0))
    path.lineTo(QPointF(100, 0))
    return StrokeItem(path, QPen(QColor("#FF0000"), width), is_highlighter=highlighter)


def _rect(width=4.0):
    from canvas.items import RectItem
    return RectItem(QRectF(0, 0, 100, 60), QPen(QColor("#FF0000"), width))


def _text(point_size=16):
    from canvas.items import TextItem
    return TextItem("abc", QPointF(0, 0), QFont("Arial", point_size), QColor("#FF0000"))


# ============================================================================
# 粗细
# ============================================================================

class TestApplySizeChange:
    """_apply_size_change_to_selection"""

    def test_scales_the_selected_item(self, view):
        item = _select(view, _stroke(width=4.0))
        view._apply_size_change_to_selection(2.0)
        assert item.get_stroke_width() == pytest.approx(8.0, abs=1e-6)

    def test_does_nothing_without_a_selection(self, view):
        """没选中任何东西时调粗细不应炸，也不应影响场景里其它图元"""
        untouched = _stroke(width=4.0)
        view.canvas_scene.addItem(untouched)
        view.smart_edit_controller.selected_item = None

        view._apply_size_change_to_selection(2.0)

        assert untouched.get_stroke_width() == pytest.approx(4.0)

    @pytest.mark.parametrize("bad_scale", [0, -1, -0.5])
    def test_non_positive_scale_is_rejected(self, view, bad_scale):
        """缩放系数为 0 或负数会把图形弄没，应当被挡住"""
        item = _select(view, _stroke(width=4.0))
        view._apply_size_change_to_selection(bad_scale)
        assert item.get_stroke_width() == pytest.approx(4.0)

    def test_repeated_scaling_never_collapses_below_one_pixel(self, view):
        item = _select(view, _stroke(width=4.0))
        for _ in range(20):
            view._apply_size_change_to_selection(0.5)
        assert item.get_stroke_width() >= 1.0

    def test_text_item_is_scaled_by_font_size(self, view):
        """文字图元没有笔宽，缩放要落在字号上"""
        item = _select(view, _text(point_size=16))
        before = view._get_text_point_size(item)

        view._apply_size_change_to_selection(2.0)

        assert view._get_text_point_size(item) == pytest.approx(before * 2, abs=0.5)

    def test_text_font_size_has_a_floor(self, view):
        """字号被缩到 0 会让文字彻底消失，应有下限"""
        item = _select(view, _text(point_size=16))
        for _ in range(20):
            view._apply_size_change_to_selection(0.5)
        assert view._get_text_point_size(item) >= 6.0


class TestScaleItemSize:
    """_scale_item_size 的分发"""

    def test_uses_the_uniform_interface_when_available(self, view):
        item = _stroke(width=4.0)
        assert view._scale_item_size(item, 3.0) is True
        assert item.get_stroke_width() == pytest.approx(12.0, abs=1e-6)

    def test_plain_graphics_text_item_falls_back_to_font_scaling(self, view):
        """非本项目 TextItem 的原生 QGraphicsTextItem 走兜底分支"""
        from PySide6.QtWidgets import QGraphicsTextItem
        item = QGraphicsTextItem("abc")
        font = item.font()
        font.setPointSizeF(10)
        item.setFont(font)

        assert view._scale_item_size(item, 2.0) is True
        assert item.font().pointSizeF() == pytest.approx(20, abs=0.5)

    def test_unsupported_item_reports_false(self, view):
        """既没有统一接口也不是文字的对象，应如实返回 False 而不是假装成功"""
        from PySide6.QtWidgets import QGraphicsRectItem
        assert view._scale_item_size(QGraphicsRectItem(QRectF(0, 0, 10, 10)), 2.0) is False


# ============================================================================
# 透明度
# ============================================================================

class TestApplyOpacityChange:
    """_apply_opacity_change_to_selection"""

    def test_applies_to_the_selected_item(self, view):
        item = _select(view, _stroke())
        view._apply_opacity_change_to_selection(0.5)
        assert item.get_visual_opacity() == pytest.approx(0.5, abs=0.01)

    @pytest.mark.parametrize("given,expected", [(5.0, 1.0), (-1.0, 0.0)])
    def test_value_is_clamped(self, view, given, expected):
        item = _select(view, _stroke())
        view._apply_opacity_change_to_selection(given)
        assert item.get_visual_opacity() == pytest.approx(expected, abs=0.01)

    def test_does_nothing_without_a_selection(self, view):
        untouched = _stroke()
        view.canvas_scene.addItem(untouched)
        view.smart_edit_controller.selected_item = None

        view._apply_opacity_change_to_selection(0.2)

        assert untouched.get_visual_opacity() == pytest.approx(1.0, abs=0.01)

    def test_item_level_opacity_stays_untouched(self, view):
        """透明度要落在颜色 alpha 上；图元级 opacity 一旦被改，导出会二次混合"""
        item = _select(view, _stroke())
        view._apply_opacity_change_to_selection(0.3)
        assert item.opacity() == pytest.approx(1.0)


# ============================================================================
# 线型
# ============================================================================

class TestApplyLineStyleChange:
    """_apply_line_style_change_to_selection"""

    @staticmethod
    def _is_dashed(pen):
        """
        Qt 在 setDashPattern() 之后会把 style 自动切成 CustomDashLine，
        所以判断"是不是虚线"要把两种枚举都算上。
        """
        return pen.style() in (Qt.PenStyle.DashLine, Qt.PenStyle.CustomDashLine)

    def test_dashed_style_is_applied_to_a_stroke(self, view):
        item = _select(view, _stroke())
        view._apply_line_style_change_to_selection("dashed")
        assert self._is_dashed(item.pen())
        assert item.pen().dashPattern() == [3, 2]

    def test_solid_style_is_applied_to_a_rect(self, view):
        item = _select(view, _rect())
        view._apply_line_style_change_to_selection("dashed")
        assert self._is_dashed(item.pen())

        view._apply_line_style_change_to_selection("solid")
        assert item.pen().style() == Qt.PenStyle.SolidLine

    def test_dense_dash_uses_a_tighter_pattern(self, view):
        item = _select(view, _stroke())
        view._apply_line_style_change_to_selection("dashed")
        loose = item.pen().dashPattern()

        view._apply_line_style_change_to_selection("dashed_dense")
        dense = item.pen().dashPattern()

        assert dense != loose
        assert sum(dense) < sum(loose)

    def test_highlighter_ignores_line_style(self, view):
        """荧光笔是实心色块，做成虚线会变得很奇怪，应当被跳过"""
        item = _select(view, _stroke(highlighter=True))
        view._apply_line_style_change_to_selection("dashed")
        assert item.pen().style() == Qt.PenStyle.SolidLine

    def test_text_item_ignores_line_style(self, view):
        """文字没有线型概念，不应因为调线型而报错"""
        item = _select(view, _text())
        view._apply_line_style_change_to_selection("dashed")   # 不抛异常即可
        assert item.toPlainText() == "abc"

    def test_does_nothing_without_a_selection(self, view):
        view.smart_edit_controller.selected_item = None
        view._apply_line_style_change_to_selection("dashed")   # 不抛异常即可


# ============================================================================
# 读取选中项的属性（工具栏回显）
# ============================================================================

class TestExtractSelectionProperties:
    """选中图元后，工具栏要把它当前的粗细/透明度回显出来"""

    def test_width_is_read_back_from_the_item(self, view):
        item = _stroke(width=7.0)
        assert view._extract_selection_width(item) == pytest.approx(7.0, abs=1e-6)

    def test_opacity_is_read_back_from_the_item(self, view):
        item = _stroke()
        item.set_visual_opacity(0.6)
        assert view._extract_selection_opacity(item) == pytest.approx(0.6, abs=0.01)

    def test_round_trip_through_apply_and_extract(self, view):
        """改完再读应当拿回同一个值，否则工具栏数字会和实际效果对不上"""
        item = _select(view, _stroke(width=4.0))

        view._apply_size_change_to_selection(2.0)
        view._apply_opacity_change_to_selection(0.25)

        assert view._extract_selection_width(item) == pytest.approx(8.0, abs=1e-6)
        assert view._extract_selection_opacity(item) == pytest.approx(0.25, abs=0.01)


# ============================================================================
# 清理
# ============================================================================

class TestCleanup:
    """cleanup 的幂等性——窗口关闭路径上会被多次调用"""

    def test_cleanup_is_idempotent(self, qapp):
        from canvas.scene import CanvasScene
        from canvas.view import CanvasView
        bg = QImage(100, 100, QImage.Format.Format_ARGB32)
        bg.fill(0xFFFFFFFF)
        v = CanvasView(CanvasScene(bg, QRectF(0, 0, 100, 100)))

        v.cleanup()
        v.cleanup()      # 第二次不应抛异常

    def test_signals_are_ignored_after_cleanup(self, qapp):
        """清理后场景若仍发来信号（异步残留），不应崩溃"""
        from canvas.scene import CanvasScene
        from canvas.view import CanvasView
        bg = QImage(100, 100, QImage.Format.Format_ARGB32)
        bg.fill(0xFFFFFFFF)
        v = CanvasView(CanvasScene(bg, QRectF(0, 0, 100, 100)))
        v.cleanup()

        v._on_cursor_tool_update_requested("pen", True)
        v._on_item_auto_select_requested(None)
