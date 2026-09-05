# -*- coding: utf-8 -*-
"""
贴图窗口的缩放、透明度与拖拽测试（pin/pin_window.py）

pin_window.py 有 580 条语句、覆盖率 16%，是 pin 包最大的窟窿。它的 __init__
会建画布、建 CanvasView、建 OCR 管理器、注册到快捷键单例、并且无条件 show()，
真实构造要打十几个桩，性价比很低。

但它最核心的交互算术全部内联在 wheelEvent 里——缩放倍率的上下限钳制、100%
吸附、透明度步长与边界——这些是用户每天都在碰的路径，算错的表现是窗口缩到
看不见、或者放大到超出屏幕。拖拽位移换算同理。这些逻辑只读 self 上的几个数值，
不需要窗口真的存在。

隔离方式：以未绑定方式调用真实实现，用 SimpleNamespace 充当 self。
这里必须用 SimpleNamespace 而不是 MagicMock，有两个原因：源码用
hasattr(self, '_image_transform') 决定基准尺寸从哪来，MagicMock 会让它恒为真；
而且 _thumbnail_mode 等是类上的 property，假 self 用普通属性才能绕开它们。
"""
from types import SimpleNamespace

import pytest
from PySide6.QtCore import QPoint, QSize, Qt

from pin.pin_window import PinWindow

NO_MOD = Qt.KeyboardModifier.NoModifier
CTRL = Qt.KeyboardModifier.ControlModifier

BASE_W, BASE_H = 400, 300
# 源码用 max(50/宽, 50/高) 保证窗口任一边不小于 50 像素
MIN_SCALE = max(50.0 / BASE_W, 50.0 / BASE_H)


class _FakeWheelEvent:
    def __init__(self, delta, modifiers=NO_MOD):
        self._delta = delta
        self._mods = modifiers
        self.ignored = 0

    def angleDelta(self):
        return QPoint(0, self._delta)

    def modifiers(self):
        return self._mods

    def ignore(self):
        self.ignored += 1


class _Recorder:
    def __init__(self):
        self.calls = []

    def __call__(self, *args):
        self.calls.append(args)

    @property
    def called(self):
        return bool(self.calls)


def _zoom_self(scale_factor=1.0, transform_size=None, thumbnail=False):
    """
    组装一个只够 wheelEvent 缩放分支使用的假窗口。

    不设 canvas（源码 `if self.canvas` 判空后跳过缓存失效），
    transform_size 为 None 时也不设 _image_transform，让 hasattr 走 else 分支
    从 _orig_size 取基准尺寸。
    """
    fake = SimpleNamespace(
        _thumbnail_mode=thumbnail,
        _is_scaling=False,
        _orig_size=QSize(BASE_W, BASE_H),
        scale_factor=scale_factor,
        canvas=None,
        x=lambda: 10,
        y=lambda: 20,
        setGeometry=_Recorder(),
        update=_Recorder(),
        _scale_timer=SimpleNamespace(start=_Recorder()),
        _show_zoom_percent=_Recorder(),
    )
    if transform_size is not None:
        fake._image_transform = SimpleNamespace(
            display_size=lambda orig: transform_size)
    return fake


def _opacity_self(opacity=1.0, thumbnail=False):
    return SimpleNamespace(
        _thumbnail_mode=thumbnail,
        _win_opacity=opacity,
        setWindowOpacity=_Recorder(),
        _show_hint_label=_Recorder(),
    )


def _scroll(fake, times, delta=120, modifiers=NO_MOD):
    for _ in range(times):
        PinWindow.wheelEvent(fake, _FakeWheelEvent(delta, modifiers))


# ============================================================================
# 缩放
# ============================================================================

class TestZoomClamping:

    def test_zooming_in_saturates_at_four_times(self):
        fake = _zoom_self()
        _scroll(fake, 40)
        assert fake.scale_factor == 4.0

    def test_zooming_out_saturates_at_the_minimum_scale(self):
        """下限保证窗口最短边不小于 50 像素，否则贴图会缩成看不见的一点"""
        fake = _zoom_self()
        _scroll(fake, 40, delta=-120)
        assert fake.scale_factor == pytest.approx(MIN_SCALE)

    def test_the_minimum_scale_follows_the_image_shape(self):
        """细长图和方图的下限不同，取宽高两个约束里更严的那个"""
        tall = _zoom_self(transform_size=QSize(1000, 60))
        _scroll(tall, 60, delta=-120)
        assert tall.scale_factor == pytest.approx(max(50.0 / 1000, 50.0 / 60))

    def test_window_never_shrinks_below_fifty_pixels_on_its_shorter_side(self):
        fake = _zoom_self()
        _scroll(fake, 40, delta=-120)
        _, _, width, height = fake.setGeometry.calls[-1]
        assert min(width, height) >= 50

    def test_a_single_step_scales_by_five_percent(self):
        fake = _zoom_self()
        _scroll(fake, 1)
        assert fake.scale_factor == pytest.approx(1.05)

    def test_zooming_out_uses_the_reciprocal_so_a_round_trip_is_lossless(self):
        """
        缩小必须用放大倍率的倒数而不是 0.95，否则放大再缩小会累积误差，
        窗口回不到原始尺寸。
        """
        fake = _zoom_self()
        _scroll(fake, 1)
        _scroll(fake, 1, delta=-120)
        assert fake.scale_factor == 1.0

    def test_passing_through_one_hundred_percent_snaps_exactly(self):
        """互逆浮点运算会在 1.0 附近留下极小误差，源码用 1e-6 阈值吸附"""
        fake = _zoom_self()
        for _ in range(5):
            _scroll(fake, 1)
        for _ in range(5):
            _scroll(fake, 1, delta=-120)
        assert fake.scale_factor == 1.0


class TestZoomGeometry:

    def test_window_size_is_computed_from_the_base_size_not_the_current_one(self):
        """
        每次都从原图逻辑尺寸乘缩放比算，避免按已取整的窗口尺寸反复取整而漂移。
        """
        fake = _zoom_self()
        _scroll(fake, 1)
        x, y, width, height = fake.setGeometry.calls[-1]
        assert (width, height) == (round(BASE_W * 1.05), round(BASE_H * 1.05))

    def test_window_position_is_preserved_while_scaling(self):
        fake = _zoom_self()
        _scroll(fake, 3)
        for x, y, _, _ in fake.setGeometry.calls:
            assert (x, y) == (10, 20)

    def test_rotated_images_scale_from_the_transformed_size(self):
        """旋转 90 度后宽高互换，缩放必须以变换后的尺寸为基准"""
        fake = _zoom_self(transform_size=QSize(BASE_H, BASE_W))
        _scroll(fake, 1)
        _, _, width, height = fake.setGeometry.calls[-1]
        assert (width, height) == (round(BASE_H * 1.05), round(BASE_W * 1.05))

    def test_size_never_degenerates_to_zero(self):
        tiny = _zoom_self(transform_size=QSize(1, 1))
        _scroll(tiny, 40, delta=-120)
        _, _, width, height = tiny.setGeometry.calls[-1]
        assert width >= 1 and height >= 1

    def test_scaling_marks_the_smoothing_flag_and_arms_the_timer(self):
        """滚动中用快速缩放，停下来后由定时器触发一次高质量重绘"""
        fake = _zoom_self()
        _scroll(fake, 1)
        assert fake._is_scaling is True
        assert fake._scale_timer.start.called
        assert fake._show_zoom_percent.called
        assert fake.update.called

    def test_the_canvas_cache_is_invalidated_when_a_canvas_exists(self):
        fake = _zoom_self()
        invalidate = _Recorder()
        fake.canvas = SimpleNamespace(invalidate_cache=invalidate)
        _scroll(fake, 1)
        assert invalidate.called


class TestZoomPercentLabel:

    def test_percentage_is_rounded_not_truncated(self):
        cases = {1.0: "100%", 1.05: "105%", 0.16666666666666666: "17%",
                 4.0: "400%", 0.999: "100%"}
        for scale, expected in cases.items():
            label = _Recorder()
            fake = SimpleNamespace(scale_factor=scale, _show_hint_label=label)
            PinWindow._show_zoom_percent(fake)
            assert label.calls == [(expected,)], scale


# ============================================================================
# 透明度
# ============================================================================

class TestOpacity:

    def test_ctrl_scroll_down_reduces_opacity_by_five_percent(self):
        fake = _opacity_self(1.0)
        _scroll(fake, 1, delta=-120, modifiers=CTRL)
        assert fake._win_opacity == pytest.approx(0.95)
        assert fake.setWindowOpacity.calls == [(pytest.approx(0.95),)]

    def test_opacity_is_clamped_to_fully_opaque(self):
        fake = _opacity_self(1.0)
        _scroll(fake, 5, modifiers=CTRL)
        assert fake._win_opacity == 1.0

    def test_opacity_is_clamped_at_fifteen_percent(self):
        """再往下就几乎看不见了，下限保证窗口不会被误操作成隐形"""
        fake = _opacity_self(0.15)
        _scroll(fake, 5, delta=-120, modifiers=CTRL)
        assert fake._win_opacity == 0.15

    def test_opacity_walks_down_and_back_up(self):
        fake = _opacity_self(1.0)
        _scroll(fake, 4, delta=-120, modifiers=CTRL)
        assert fake._win_opacity == pytest.approx(0.80)
        _scroll(fake, 2, modifiers=CTRL)
        assert fake._win_opacity == pytest.approx(0.90)

    def test_ctrl_scroll_does_not_touch_the_scale(self):
        fake = _opacity_self(1.0)
        fake.scale_factor = 1.0
        fake.setGeometry = _Recorder()
        _scroll(fake, 1, modifiers=CTRL)
        assert fake.scale_factor == 1.0
        assert fake.setGeometry.calls == []

    def test_opacity_label_truncates_instead_of_rounding(self):
        """
        已知缺陷，不是回归：透明度标签用 int(_win_opacity * 100) 截断，
        而累积浮点误差让 0.9 变成 0.8999999999999999，标签因此显示 89%。
        同文件的 _show_zoom_percent 用的是 int(round(...))，两处不一致。
        修成 round 之后这条用例会失败，届时应把预期改成 90%。
        """
        fake = _opacity_self(1.0)
        _scroll(fake, 2, delta=-120, modifiers=CTRL)
        assert fake._win_opacity == pytest.approx(0.90)
        assert fake._show_hint_label.calls[-1] == ("α 89%",)

    def test_the_first_step_down_happens_to_label_correctly(self):
        fake = _opacity_self(1.0)
        _scroll(fake, 1, delta=-120, modifiers=CTRL)
        assert fake._show_hint_label.calls[-1] == ("α 95%",)


# ============================================================================
# 缩略图模式下滚轮被忽略
# ============================================================================

class TestThumbnailModeIgnoresWheel:

    def test_scale_is_left_untouched(self):
        fake = _zoom_self(thumbnail=True)
        event = _FakeWheelEvent(120)
        PinWindow.wheelEvent(fake, event)
        assert fake.scale_factor == 1.0
        assert fake.setGeometry.calls == []
        assert event.ignored == 1

    def test_ctrl_scroll_is_ignored_too(self):
        fake = _opacity_self(1.0, thumbnail=True)
        event = _FakeWheelEvent(-120, CTRL)
        PinWindow.wheelEvent(fake, event)
        assert fake._win_opacity == 1.0
        assert event.ignored == 1
        assert fake.setWindowOpacity.calls == []


# ============================================================================
# 拖拽
# ============================================================================

def _drag_self(dragging=False, start_pos=QPoint(100, 100),
               window_pos=QPoint(500, 400)):
    return SimpleNamespace(
        _is_dragging=dragging,
        _drag_start_pos=start_pos,
        _drag_start_window_pos=window_pos,
        toolbar=None,
        move=_Recorder(),
        setCursor=_Recorder(),
        pos=lambda: window_pos,
    )


class TestWindowDrag:

    def test_starting_a_drag_records_the_anchor_and_changes_the_cursor(self):
        fake = _drag_self()
        PinWindow.start_window_drag(fake, QPoint(150, 160))
        assert fake._is_dragging is True
        assert fake._drag_start_pos == QPoint(150, 160)
        assert fake._drag_start_window_pos == QPoint(500, 400)
        assert fake.setCursor.calls == [(Qt.CursorShape.ClosedHandCursor,)]

    def test_the_window_follows_the_pointer_delta(self):
        fake = _drag_self(dragging=True)
        PinWindow.update_window_drag(fake, QPoint(130, 90))
        # 指针右移 30、上移 10，窗口同量移动
        assert fake.move.calls == [(QPoint(530, 390),)]

    def test_several_moves_are_all_measured_from_the_original_anchor(self):
        """位移始终相对按下时的锚点算，而不是累加上一次的增量"""
        fake = _drag_self(dragging=True)
        for point, expected in ((QPoint(110, 110), QPoint(510, 410)),
                                (QPoint(120, 120), QPoint(520, 420)),
                                (QPoint(90, 95), QPoint(490, 395))):
            PinWindow.update_window_drag(fake, point)
            assert fake.move.calls[-1] == (expected,)

    def test_moving_without_a_started_drag_does_nothing(self):
        fake = _drag_self(dragging=False)
        PinWindow.update_window_drag(fake, QPoint(999, 999))
        assert fake.move.calls == []

    def test_ending_a_drag_restores_the_cursor(self):
        fake = _drag_self(dragging=True)
        PinWindow.end_window_drag(fake)
        assert fake._is_dragging is False
        assert fake.setCursor.calls == [(Qt.CursorShape.ArrowCursor,)]

    def test_ending_a_drag_that_never_started_is_a_no_op(self):
        fake = _drag_self(dragging=False)
        PinWindow.end_window_drag(fake)
        assert fake.setCursor.calls == []

    def test_a_visible_toolbar_is_kept_in_sync(self):
        sync = _Recorder()
        fake = _drag_self(dragging=True)
        fake.toolbar = SimpleNamespace(
            isVisible=lambda: True, sync_with_pin_window=sync)
        PinWindow.update_window_drag(fake, QPoint(110, 110))
        assert sync.called

    def test_a_hidden_toolbar_is_not_synced(self):
        sync = _Recorder()
        fake = _drag_self(dragging=True)
        fake.toolbar = SimpleNamespace(
            isVisible=lambda: False, sync_with_pin_window=sync)
        PinWindow.update_window_drag(fake, QPoint(110, 110))
        assert not sync.called


# ============================================================================
# 依赖子管理器的属性回退
# ============================================================================

class TestManagerBackedProperties:
    """
    这四个属性都写成 `self._xxx.yyy if hasattr(self, '_xxx') else 兜底`，
    保护窗口在子管理器还没建好或已清理时不至于抛 AttributeError。
    property 是类上的描述符，用 fget 直接喂假 self 即可测。
    """

    def test_values_are_read_through_to_the_ocr_manager(self):
        layer = object()
        fake = SimpleNamespace(_ocr_mgr=SimpleNamespace(
            ocr_text_layer=layer, has_result=True, text_selection_enabled=True))
        assert PinWindow.ocr_text_layer.fget(fake) is layer
        assert PinWindow._ocr_has_result.fget(fake) is True
        assert PinWindow._text_selection_enabled.fget(fake) is True

    def test_a_missing_ocr_manager_falls_back_safely(self):
        bare = SimpleNamespace()
        assert PinWindow.ocr_text_layer.fget(bare) is None
        assert PinWindow._ocr_has_result.fget(bare) is False
        assert PinWindow._text_selection_enabled.fget(bare) is False

    def test_thumbnail_state_is_read_through_to_its_manager(self):
        for active in (True, False):
            fake = SimpleNamespace(_thumbnail=SimpleNamespace(active=active))
            assert PinWindow._thumbnail_mode.fget(fake) is active

    def test_a_missing_thumbnail_manager_reports_inactive(self):
        assert PinWindow._thumbnail_mode.fget(SimpleNamespace()) is False
