# -*- coding: utf-8 -*-
"""
钉图缩略图模式测试

PinThumbnailMode 把钉图缩成鼠标位置上的一个小方块，再按原样还原。
它对 PinWindow 的依赖都是很窄的接口调用，所以可以用假窗口驱动，
不必创建真实的置顶窗口。

要紧的行为：
- 进入时以鼠标所在的画面位置为取景中心；鼠标不在窗口上时退回画面中心
- 取景框必须被夹在图像范围内，否则缩略图里会露出图像外的空白
- 进入 → 退出应还原成原来的尺寸，且画面中心仍落在同一处
- 进入时要退出编辑状态、关掉 OCR 层与工具栏，退出时再恢复，
  否则缩略图上会浮着一个跟窗口一样大的工具栏
"""
import pytest
from PySide6.QtCore import QPoint, QRect, QSize, QRectF
from PySide6.QtGui import QCursor
from PySide6.QtWidgets import QApplication


@pytest.fixture(scope="module")
def qapp():
    app = QApplication.instance()
    if app is None:
        app = QApplication([])
    yield app


class FakeView:
    def __init__(self):
        self.reset_calls = 0
        self.fitted_rects = []

    def resetTransform(self):
        self.reset_calls += 1

    def fitInView(self, rect, mode):
        self.fitted_rects.append(QRectF(rect))


class FakeCanvas:
    def __init__(self, is_editing=False):
        self.is_editing = is_editing
        self.deactivated = 0

    def deactivate_tool(self):
        self.deactivated += 1
        self.is_editing = False


class FakeToolbar:
    def __init__(self, visible=True):
        self._visible = visible
        self.current_tool = None
        self.tool_buttons = {}
        self.hidden = 0
        self.panels_hidden = 0

    def isVisible(self):
        return self._visible

    def hide(self):
        self._visible = False
        self.hidden += 1

    def _hide_all_panels(self):
        self.panels_hidden += 1


class FakeOCRManager:
    def __init__(self):
        self.enabled_calls = []
        self.geometry_updates = []

    def set_enabled(self, enabled):
        self.enabled_calls.append(enabled)

    def update_geometry(self, rect):
        self.geometry_updates.append(rect)


class FakePinWindow:
    """只实现 PinThumbnailMode 用到的那部分 PinWindow 接口"""

    def __init__(self, geometry=QRect(200, 100, 400, 300),
                 orig_size=QSize(800, 600), editing=False, toolbar=True):
        self._geometry = QRect(geometry)
        self._orig_size = orig_size
        self.view = FakeView()
        self.canvas = FakeCanvas(editing)
        self.toolbar = FakeToolbar() if toolbar else None
        self._ocr_mgr = FakeOCRManager()

        self.control_visibility = []
        self.transform_updates = 0
        self.button_position_updates = 0

    # -- 几何 --
    def geometry(self):
        return QRect(self._geometry)

    def setGeometry(self, x, y, w, h):
        self._geometry = QRect(x, y, w, h)

    def mapFromGlobal(self, pos):
        return QPoint(pos.x() - self._geometry.x(), pos.y() - self._geometry.y())

    def content_rect(self):
        return QRectF(0, 0, self._geometry.width(), self._geometry.height())

    # -- 回调 --
    def _set_control_buttons_visible(self, visible):
        self.control_visibility.append(visible)

    def _update_view_transform(self):
        self.transform_updates += 1

    def update_button_positions(self):
        self.button_position_updates += 1


@pytest.fixture
def mode(qapp):
    from pin.pin_thumbnail import PinThumbnailMode

    def _make(win=None, **kwargs):
        return PinThumbnailMode(win or FakePinWindow(**kwargs))

    return _make


@pytest.fixture
def cursor_at(monkeypatch):
    def _set(x, y):
        monkeypatch.setattr(QCursor, "pos", staticmethod(lambda: QPoint(x, y)))
    return _set


# ============================================================================
# 初始状态
# ============================================================================

class TestInitialState:
    def test_starts_inactive(self, mode):
        assert mode().active is False

    def test_default_thumbnail_size(self, mode):
        assert mode().size == 100


# ============================================================================
# 进入缩略图模式
# ============================================================================

class TestEnter:
    def test_toggle_activates_the_mode(self, mode, cursor_at):
        cursor_at(400, 250)
        m = mode()
        m.toggle()
        assert m.active is True

    def test_window_shrinks_to_the_thumbnail_size(self, mode, cursor_at):
        cursor_at(400, 250)
        win = FakePinWindow()
        m = mode(win)

        m.toggle()

        assert win.geometry().size() == QSize(100, 100)

    def test_thumbnail_is_centred_on_the_cursor(self, mode, cursor_at):
        cursor_at(400, 250)
        win = FakePinWindow()
        m = mode(win)

        m.toggle()

        assert win.geometry().center() in (QPoint(400, 250), QPoint(399, 249))

    def test_cursor_inside_the_window_picks_that_part_of_the_image(self, mode, cursor_at):
        """
        窗口 400x300 显示的是 800x600 的原图，鼠标停在窗口正中，
        取景中心就应落在原图正中。
        """
        win = FakePinWindow(geometry=QRect(200, 100, 400, 300),
                            orig_size=QSize(800, 600))
        cursor_at(200 + 200, 100 + 150)
        m = mode(win)

        m.toggle()

        assert m._scene_center.x() == pytest.approx(400, abs=1)
        assert m._scene_center.y() == pytest.approx(300, abs=1)

    def test_cursor_outside_the_window_falls_back_to_the_image_centre(self, mode, cursor_at):
        win = FakePinWindow(geometry=QRect(200, 100, 400, 300),
                            orig_size=QSize(800, 600))
        cursor_at(5000, 5000)
        m = mode(win)

        m.toggle()

        assert m._scene_center.x() == pytest.approx(400, abs=1)
        assert m._scene_center.y() == pytest.approx(300, abs=1)

    def test_entering_disables_the_ocr_layer(self, mode, cursor_at):
        """缩到 100px 时 OCR 文字层没有意义，还会挡住鼠标事件"""
        cursor_at(400, 250)
        win = FakePinWindow()
        mode(win).toggle()

        assert win._ocr_mgr.enabled_calls == [False]

    def test_entering_leaves_edit_mode(self, mode, cursor_at):
        cursor_at(400, 250)
        win = FakePinWindow(editing=True)
        win.toolbar.current_tool = "pen"
        mode(win).toggle()

        assert win.canvas.deactivated == 1
        assert win.toolbar.current_tool is None

    def test_entering_hides_the_toolbar_and_its_panels(self, mode, cursor_at):
        cursor_at(400, 250)
        win = FakePinWindow()
        mode(win).toggle()

        assert win.toolbar.hidden == 1
        assert win.toolbar.panels_hidden == 1

    def test_entering_hides_the_control_buttons(self, mode, cursor_at):
        cursor_at(400, 250)
        win = FakePinWindow()
        mode(win).toggle()

        assert win.control_visibility == [False]

    def test_entering_works_without_a_toolbar(self, mode, cursor_at):
        cursor_at(400, 250)
        win = FakePinWindow(toolbar=False)
        mode(win).toggle()          # 不应抛异常


# ============================================================================
# 取景范围
# ============================================================================

class TestViewportClamping:
    """
    缩略图显示的是原图上的一个 100x100 方块。
    这个方块必须完整落在图像内，否则会露出图像外的空白。
    """

    def _enter_at(self, mode, cursor_at, cursor, orig=QSize(800, 600)):
        win = FakePinWindow(geometry=QRect(200, 100, 400, 300), orig_size=orig)
        cursor_at(*cursor)
        m = mode(win)
        m.toggle()
        return win, m

    def test_viewport_is_inside_the_image_for_a_centre_cursor(self, mode, cursor_at):
        win, _m = self._enter_at(mode, cursor_at, (400, 250))
        rect = win.view.fitted_rects[-1]
        assert rect.left() >= 0 and rect.top() >= 0
        assert rect.right() <= 800 and rect.bottom() <= 600

    def test_cursor_at_the_top_left_clamps_the_viewport_to_the_corner(self, mode, cursor_at):
        win, _m = self._enter_at(mode, cursor_at, (201, 101))
        rect = win.view.fitted_rects[-1]
        assert rect.left() == pytest.approx(0)
        assert rect.top() == pytest.approx(0)

    def test_cursor_at_the_bottom_right_clamps_the_viewport(self, mode, cursor_at):
        win, _m = self._enter_at(mode, cursor_at, (599, 399))
        rect = win.view.fitted_rects[-1]
        assert rect.right() == pytest.approx(800)
        assert rect.bottom() == pytest.approx(600)

    def test_viewport_is_square_and_thumbnail_sized(self, mode, cursor_at):
        win, m = self._enter_at(mode, cursor_at, (400, 250))
        rect = win.view.fitted_rects[-1]
        assert rect.width() == m.size
        assert rect.height() == m.size

    def test_image_smaller_than_the_thumbnail_still_starts_at_the_origin(self, mode, cursor_at):
        """原图比缩略图还小时，取景框只能从 (0,0) 开始"""
        win, _m = self._enter_at(mode, cursor_at, (400, 250), orig=QSize(50, 40))
        rect = win.view.fitted_rects[-1]
        assert rect.left() == pytest.approx(0)
        assert rect.top() == pytest.approx(0)

    def test_scene_centre_is_updated_to_the_clamped_position(self, mode, cursor_at):
        """取景框被夹住后，记录的中心点也要跟着修正，否则退出时会跳位"""
        _win, m = self._enter_at(mode, cursor_at, (201, 101))
        assert m._scene_center.x() == pytest.approx(50)
        assert m._scene_center.y() == pytest.approx(50)


# ============================================================================
# 退出缩略图模式
# ============================================================================

class TestExit:
    def test_toggle_twice_returns_to_normal(self, mode, cursor_at):
        cursor_at(400, 250)
        m = mode()
        m.toggle()
        m.toggle()
        assert m.active is False

    def test_original_size_is_restored(self, mode, cursor_at):
        cursor_at(400, 250)
        win = FakePinWindow(geometry=QRect(200, 100, 400, 300))
        m = mode(win)

        m.toggle()
        m.toggle()

        assert win.geometry().size() == QSize(400, 300)

    def test_exiting_restores_the_ocr_layer(self, mode, cursor_at):
        cursor_at(400, 250)
        win = FakePinWindow()
        m = mode(win)

        m.toggle()
        m.toggle()

        assert win._ocr_mgr.enabled_calls == [False, True]
        assert len(win._ocr_mgr.geometry_updates) == 1

    def test_exiting_restores_the_control_buttons(self, mode, cursor_at):
        cursor_at(400, 250)
        win = FakePinWindow()
        m = mode(win)

        m.toggle()
        m.toggle()

        assert win.control_visibility == [False, True]
        assert win.button_position_updates == 1

    def test_exit_state_is_cleared(self, mode, cursor_at):
        cursor_at(400, 250)
        m = mode()
        m.toggle()
        m.toggle()
        assert m._scene_center is None
        assert m._prev_geometry is None

    def test_exiting_without_having_entered_is_safe(self, mode):
        """状态不完整时直接退出只应复位标志，不能抛异常"""
        m = mode()
        m._active = True
        m._exit()
        assert m.active is False

    def test_the_viewed_point_stays_put_across_a_round_trip(self, mode, cursor_at):
        """
        缩起来再放开，原先鼠标指着的画面位置应仍在窗口的同一相对处，
        否则每次切换缩略图画面都会漂移。
        """
        win = FakePinWindow(geometry=QRect(200, 100, 400, 300),
                            orig_size=QSize(800, 600))
        cursor_at(200 + 100, 100 + 75)      # 窗口内 1/4 处 → 原图 (200, 150)
        m = mode(win)

        m.toggle()
        thumb_centre = win.geometry().center()
        m.toggle()

        restored = win.geometry()
        # 该点在原图上的相对位置是 (0.25, 0.25)，还原后它应仍落在缩略图中心处
        expected_x = thumb_centre.x() - 0.25 * restored.width()
        expected_y = thumb_centre.y() - 0.25 * restored.height()
        assert restored.x() == pytest.approx(expected_x, abs=2)
        assert restored.y() == pytest.approx(expected_y, abs=2)
