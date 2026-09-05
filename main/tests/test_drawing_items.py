# -*- coding: utf-8 -*-
"""
canvas.items 图元测试

drawing_items.py 是画布里最大的一个文件（近 1700 行），此前只有序号图元
被顺带测到几行。这里覆盖各图元真正被反复调用的那部分：

- 所有图元共用的"统一属性接口"（笔宽 / 视觉透明度）的跨类一致性
  ——这套接口被工具栏、撤销命令、缩放逻辑同时调用，任何一个图元实现漂移
  都会表现成"改了粗细某个图形不跟着变"
- shape()：点击热区。线画得再细也要能点中，否则用户选不中自己画的东西
- ArrowItem 的直线/曲线状态机与控制点跟随规则
"""
import pytest
from PySide6.QtWidgets import QApplication
from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import QColor, QPen, QPainterPath, QFont


@pytest.fixture(scope="module")
def qapp():
    app = QApplication.instance()
    if app is None:
        app = QApplication([])
    yield app


def _pen(width=3.0, color="#FF0000"):
    return QPen(QColor(color), width)


def _stroke_item(width=3.0, highlighter=False):
    from canvas.items import StrokeItem
    path = QPainterPath(QPointF(0, 0))
    path.lineTo(QPointF(100, 0))
    return StrokeItem(path, _pen(width), is_highlighter=highlighter)


def _rect_item(width=3.0):
    from canvas.items import RectItem
    return RectItem(QRectF(0, 0, 100, 60), _pen(width))


def _ellipse_item(width=3.0):
    from canvas.items import EllipseItem
    return EllipseItem(QRectF(0, 0, 100, 60), _pen(width))


def _arrow_item(width=3.0, style="single"):
    from canvas.items import ArrowItem
    return ArrowItem(QPointF(0, 0), QPointF(100, 50), _pen(width), style)


def _number_item(radius=20.0):
    from canvas.items import NumberItem
    return NumberItem(1, QPointF(50, 50), radius, QColor("#FF0000"))


ALL_ITEM_FACTORIES = [
    ("StrokeItem", _stroke_item),
    ("RectItem", _rect_item),
    ("EllipseItem", _ellipse_item),
    ("ArrowItem", _arrow_item),
    ("NumberItem", _number_item),
]


# ============================================================================
# 跨图元的统一属性接口
# ============================================================================

class TestUniformStrokeWidthInterface:
    """
    每个图元都实现了 set_stroke_width / scale_stroke_width / get_stroke_width。
    工具栏调粗细时是对着选中的任意图元调用的，所以这套接口必须跨类同构。
    """

    @pytest.mark.parametrize("name,factory", ALL_ITEM_FACTORIES)
    def test_every_item_exposes_the_interface(self, qapp, name, factory):
        item = factory()
        for method in ("set_stroke_width", "scale_stroke_width",
                       "set_visual_opacity", "get_stroke_width",
                       "get_visual_opacity"):
            assert callable(getattr(item, method)), f"{name} 缺少 {method}"

    @pytest.mark.parametrize("name,factory", [
        ("StrokeItem", _stroke_item),
        ("RectItem", _rect_item),
        ("EllipseItem", _ellipse_item),
        ("ArrowItem", _arrow_item),
    ])
    def test_set_then_get_round_trips(self, qapp, name, factory):
        item = factory()
        item.set_stroke_width(8)
        assert item.get_stroke_width() == pytest.approx(8, abs=1e-6), name

    @pytest.mark.parametrize("name,factory", [
        ("StrokeItem", _stroke_item),
        ("RectItem", _rect_item),
        ("EllipseItem", _ellipse_item),
        ("ArrowItem", _arrow_item),
    ])
    def test_width_never_drops_below_one_pixel(self, qapp, name, factory):
        """笔宽被压到 0 会让图形彻底看不见，各图元都应钳制到 >= 1"""
        item = factory()
        item.set_stroke_width(0)
        assert item.get_stroke_width() >= 1.0, name

        item.set_stroke_width(-5)
        assert item.get_stroke_width() >= 1.0, name

    @pytest.mark.parametrize("name,factory", [
        ("StrokeItem", _stroke_item),
        ("RectItem", _rect_item),
        ("EllipseItem", _ellipse_item),
        ("ArrowItem", _arrow_item),
    ])
    def test_scale_multiplies_current_width(self, qapp, name, factory):
        item = factory(width=4.0)
        assert item.scale_stroke_width(2.0) is True
        assert item.get_stroke_width() == pytest.approx(8, abs=1e-6), name

    @pytest.mark.parametrize("name,factory", [
        ("StrokeItem", _stroke_item),
        ("RectItem", _rect_item),
        ("EllipseItem", _ellipse_item),
        ("ArrowItem", _arrow_item),
    ])
    def test_scaling_down_hard_is_still_clamped(self, qapp, name, factory):
        """缩小到极限时同样不能跌破 1px"""
        item = factory(width=4.0)
        item.scale_stroke_width(0.001)
        assert item.get_stroke_width() >= 1.0, name

    def test_highlighter_reports_a_third_of_its_pen_width(self, qapp):
        """
        荧光笔用三倍笔宽绘制来做粗描边，但对外汇报的应是用户设定的粗细，
        否则工具栏上的数字每次选中荧光笔都会翻三倍。
        """
        item = _stroke_item(width=9.0, highlighter=True)
        assert item.get_stroke_width() == pytest.approx(3.0)


class TestUniformOpacityInterface:
    """视觉透明度接口的跨类一致性"""

    @pytest.mark.parametrize("name,factory", ALL_ITEM_FACTORIES)
    def test_set_then_get_round_trips(self, qapp, name, factory):
        item = factory()
        assert item.set_visual_opacity(0.5) is True
        assert item.get_visual_opacity() == pytest.approx(0.5, abs=0.01), name

    @pytest.mark.parametrize("name,factory", ALL_ITEM_FACTORIES)
    def test_opacity_is_clamped_into_zero_one(self, qapp, name, factory):
        item = factory()
        item.set_visual_opacity(5.0)
        assert item.get_visual_opacity() == pytest.approx(1.0, abs=0.01), name

        item.set_visual_opacity(-1.0)
        assert item.get_visual_opacity() == pytest.approx(0.0, abs=0.01), name

    @pytest.mark.parametrize("name,factory", ALL_ITEM_FACTORIES)
    def test_opacity_lives_on_the_paint_color_not_on_the_item(self, qapp, name, factory):
        """
        透明度必须落在绘制用的颜色 alpha 上，图元自身的 opacity 保持 1。
        若改用图元级 setOpacity，导出时半透明图形会和背景二次混合。

        各图元存放颜色的位置不同（StrokeItem/RectItem/EllipseItem 存在 pen 上，
        ArrowItem 存在自己的 color 属性上，因为箭头头部还要用同一颜色填充），
        所以这里只约束"不用图元级透明度"这个共同不变量，
        具体取值由 get_visual_opacity 的往返测试覆盖。
        """
        item = factory()
        item.set_visual_opacity(0.4)
        assert item.opacity() == pytest.approx(1.0), name

    @pytest.mark.parametrize("name,factory", [
        ("StrokeItem", _stroke_item),
        ("RectItem", _rect_item),
        ("EllipseItem", _ellipse_item),
    ])
    def test_pen_based_items_store_alpha_on_the_pen(self, qapp, name, factory):
        """通过画笔绘制的图元，alpha 应写进 pen 的颜色里"""
        item = factory()
        item.set_visual_opacity(0.4)
        assert item.pen().color().alphaF() == pytest.approx(0.4, abs=0.01), name

    def test_arrow_stores_alpha_on_its_own_color(self, qapp):
        """箭头用统一的 color 属性同时决定描边和箭头填充"""
        arrow = _arrow_item()
        arrow.set_visual_opacity(0.4)
        assert arrow.color.alphaF() == pytest.approx(0.4, abs=0.01)

    @pytest.mark.parametrize("name,factory", ALL_ITEM_FACTORIES)
    def test_changing_width_preserves_opacity(self, qapp, name, factory):
        """先调透明度再调粗细，透明度不应被重置——两个滑块互不干扰"""
        item = factory()
        item.set_visual_opacity(0.3)
        item.set_stroke_width(7)
        assert item.get_visual_opacity() == pytest.approx(0.3, abs=0.01), name


# ============================================================================
# 点击热区
# ============================================================================

class TestClickableShape:
    """shape() 决定了图元能不能被点中"""

    def test_thin_stroke_still_has_a_generous_hit_area(self, qapp):
        """1px 细线也必须能点中，否则用户选不中自己画的线"""
        item = _stroke_item(width=1.0)
        shape = item.shape()
        # 线在 y=0 上，从上方 8px 处点击也应命中
        assert shape.contains(QPointF(50, 8))

    def test_hit_area_grows_with_pen_width(self, qapp):
        thin = _stroke_item(width=1.0).shape().boundingRect()
        thick = _stroke_item(width=20.0).shape().boundingRect()
        assert thick.height() > thin.height()

    def test_far_away_click_misses_the_stroke(self, qapp):
        """热区放宽也要有边界，不能整块画布都算命中"""
        item = _stroke_item(width=1.0)
        assert not item.shape().contains(QPointF(50, 200))

    def test_shape_cache_is_invalidated_when_path_changes(self, qapp):
        """
        StrokeItem 缓存了 shape。改路径后若不失效缓存，
        点击热区会一直停留在旧位置。
        """
        item = _stroke_item(width=2.0)
        first = item.shape().boundingRect()

        longer = QPainterPath(QPointF(0, 0))
        longer.lineTo(QPointF(300, 0))
        item.setPath(longer)

        assert item.shape().boundingRect().width() > first.width()

    def test_shape_cache_is_invalidated_when_pen_changes(self, qapp):
        item = _stroke_item(width=2.0)
        first = item.shape().boundingRect()

        item.set_stroke_width(30)

        assert item.shape().boundingRect().height() > first.height()

    @pytest.mark.parametrize("name,factory", [
        ("RectItem", _rect_item),
        ("EllipseItem", _ellipse_item),
    ])
    def test_shape_covers_the_outline_of_shape_items(self, qapp, name, factory):
        """空心图形点边框应命中（内部是否命中由各自实现决定，这里只约束边框）"""
        item = factory(width=2.0)
        assert item.shape().contains(QPointF(0, 30)), name       # 左边框上
        assert item.shape().contains(QPointF(100, 30)), name     # 右边框上

    def test_number_item_shape_matches_its_visual_circle(self, qapp):
        item = _number_item(radius=20)
        assert item.shape().contains(item.visualRect().center())


# ============================================================================
# 箭头
# ============================================================================

class TestArrowItem:
    """箭头的直线/曲线状态机"""

    def test_new_arrow_is_straight(self, qapp):
        arrow = _arrow_item()
        assert arrow.is_curved() is False

    def test_control_point_defaults_to_the_midpoint(self, qapp):
        arrow = _arrow_item()   # (0,0) -> (100,50)
        ctrl = arrow.get_control_point()
        assert ctrl.x() == pytest.approx(50)
        assert ctrl.y() == pytest.approx(25)

    def test_untouched_control_point_follows_the_endpoints(self, qapp):
        """没手动拖过控制点时，改动端点后控制点应继续待在中点"""
        arrow = _arrow_item()
        arrow.set_positions(QPointF(0, 0), QPointF(200, 100))

        ctrl = arrow.get_control_point()
        assert ctrl.x() == pytest.approx(100)
        assert ctrl.y() == pytest.approx(50)

    def test_dragging_the_control_point_makes_the_arrow_curved(self, qapp):
        arrow = _arrow_item()
        arrow.set_control_point(QPointF(50, -40))

        assert arrow.is_curved() is True
        assert arrow.get_control_point().y() == pytest.approx(-40)

    def test_dragged_control_point_keeps_its_absolute_position(self, qapp):
        """
        已手动调过弯的箭头，再拖动端点时控制点应保持在原地
        （否则用户精心调好的弧度会在移动箭头时被抹平）。
        """
        arrow = _arrow_item()
        arrow.set_control_point(QPointF(50, -40))
        arrow.set_positions(QPointF(0, 0), QPointF(200, 100))

        ctrl = arrow.get_control_point()
        assert ctrl.x() == pytest.approx(50)
        assert ctrl.y() == pytest.approx(-40)

    def test_reset_restores_a_straight_arrow_at_the_midpoint(self, qapp):
        arrow = _arrow_item()
        arrow.set_control_point(QPointF(50, -40))
        arrow.reset_control_point()

        assert arrow.is_curved() is False
        ctrl = arrow.get_control_point()
        assert ctrl.x() == pytest.approx(50)
        assert ctrl.y() == pytest.approx(25)

    def test_geometry_spans_both_endpoints(self, qapp):
        arrow = _arrow_item()
        rect = arrow.path().boundingRect()
        assert rect.width() >= 90
        assert rect.height() >= 40

    def test_style_switches_between_known_values(self, qapp):
        arrow = _arrow_item(style="single")
        assert arrow.arrow_style == "single"

        arrow.arrow_style = "double"
        assert arrow.arrow_style == "double"

    def test_unknown_style_is_rejected(self, qapp):
        """非法样式不应被写入，否则绘制时会走到没有分支处理的状态"""
        arrow = _arrow_item(style="single")
        arrow.arrow_style = "nonsense"
        assert arrow.arrow_style == "single"

    def test_zero_length_arrow_does_not_crash(self, qapp):
        """起点终点重合时（点一下没拖）几何计算不能除零崩溃"""
        arrow = _arrow_item()
        arrow.set_positions(QPointF(10, 10), QPointF(10, 10))
        assert arrow.path() is not None


# ============================================================================
# 文字与序号
# ============================================================================

class TestTextItem:
    """文字图元"""

    @pytest.fixture
    def text_item(self, qapp):
        from canvas.items import TextItem
        return TextItem("测试文字", QPointF(10, 10), QFont("Arial", 16), QColor("#FF0000"))

    def test_keeps_its_text_and_position(self, text_item):
        assert text_item.toPlainText() == "测试文字"
        assert text_item.pos().x() == pytest.approx(10)
        assert text_item.pos().y() == pytest.approx(10)

    def test_opacity_round_trips(self, text_item):
        assert text_item.set_visual_opacity(0.5) is True
        assert text_item.get_visual_opacity() == pytest.approx(0.5, abs=0.01)

    def test_outline_and_shadow_toggles_do_not_crash(self, text_item):
        text_item.set_outline(True, QColor("#000000"), 2)
        text_item.set_shadow(True, QColor("#808080"))
        text_item.set_background(True, QColor("#FFFFFF"), 200)
        assert text_item.boundingRect().isValid()


class TestNumberItem:
    """序号图元"""

    def test_visual_rect_is_centred_on_its_position(self, qapp):
        item = _number_item(radius=20)
        rect = item.visualRect()
        assert rect.width() == pytest.approx(40, abs=1)
        assert rect.height() == pytest.approx(40, abs=1)

    def test_bounding_rect_contains_the_visual_circle(self, qapp):
        """包围盒必须裹住可视圆，否则移动时会留下没被重绘的残影"""
        item = _number_item(radius=20)
        assert item.boundingRect().contains(item.visualRect())

    def test_scene_visual_rect_tracks_item_movement(self, qapp):
        item = _number_item(radius=20)
        before = item.sceneVisualRect().center()

        item.setPos(item.pos() + QPointF(100, 0))
        after = item.sceneVisualRect().center()

        assert after.x() - before.x() == pytest.approx(100, abs=1e-6)

    def test_number_value_is_kept(self, qapp):
        from canvas.items import NumberItem
        item = NumberItem(7, QPointF(0, 0), 20, QColor("#FF0000"))
        assert item.number == 7
