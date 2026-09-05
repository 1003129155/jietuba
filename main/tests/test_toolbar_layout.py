# -*- coding: utf-8 -*-
"""
工具栏定位与工具切换状态机测试（ui/toolbar.py）

toolbar.py 有 622 条语句、覆盖率约 12%。其中价值最高、又完全不需要真实屏幕的
是两段逻辑：position_near_rect 把选区矩形换算成工具栏坐标（含"下方放不下就翻到
上方"和屏幕边界钳制），以及 _on_tool_clicked 的工具选中/取消状态机。前者算错会
让工具栏跑到屏幕外，后者算错会让工具卡在选中态——都是用户立刻能看见、但现有
测试一条都不覆盖的路径。

隔离方式：不构造真的 Toolbar（__init__ 会建整套按钮、逐个读 SVG 图标、读主题和
工具配置），而是以未绑定方式调用真实实现，用一个只暴露被读取到的属性的假 self。
被测方法只做几何计算和状态赋值，不需要 QApplication。
"""
from types import SimpleNamespace

from PySide6.QtCore import QPoint, QRect, QRectF

from ui import toolbar as toolbar_mod
from ui.toolbar import Toolbar

SCREEN = QRect(0, 0, 1920, 1080)  # right() == 1919, bottom() == 1079
MARGIN = 10                       # 生产代码里的固定边距
TOOLBAR_W, TOOLBAR_H = 200, 40


class _FakeToolbar:
    """只实现 position_near_rect 会读到的那几个成员"""

    def __init__(self, panel_extra=0, screen_rect=SCREEN, manual=False, parent=None):
        self._panel_extra = panel_extra
        self._screen_rect = screen_rect
        self._manual_positioned = manual
        self._parent = parent
        self._toolbar_below_selection = None
        self.moved_to = None

    def width(self):
        return TOOLBAR_W

    def height(self):
        return TOOLBAR_H

    def _get_max_panel_height(self):
        return self._panel_extra

    def _get_screen_by_center(self, window_rect):
        return SimpleNamespace(geometry=lambda: self._screen_rect)

    def parent(self):
        return self._parent

    def move(self, pos):
        self.moved_to = pos


def _place(rect, **kwargs):
    """跑一次定位，返回假工具栏以便断言"""
    fake = _FakeToolbar(**kwargs)
    Toolbar.position_near_rect(fake, rect)
    return fake


class TestPositionBelowSelection:

    def test_toolbar_sits_below_and_right_aligned_when_there_is_room(self):
        # QRectF(500,300,400,200).toRect() → right()=899, bottom()=499
        fake = _place(QRectF(500, 300, 400, 200))
        assert fake.moved_to == QPoint(899 - TOOLBAR_W, 499 + MARGIN)
        assert fake._toolbar_below_selection is True

    def test_position_is_recorded_for_the_secondary_panels(self):
        """二级面板靠 _toolbar_below_selection 决定往上还是往下弹"""
        below = _place(QRectF(500, 300, 400, 200))
        above = _place(QRectF(500, 1000, 400, 60))
        assert below._toolbar_below_selection is True
        assert above._toolbar_below_selection is False


class TestFlipAboveSelection:

    def test_toolbar_flips_above_when_it_does_not_fit_below(self):
        # bottom()=1059 → 下方 y=1069，1069+40 超出 1079
        fake = _place(QRectF(500, 1000, 400, 60))
        assert fake._toolbar_below_selection is False
        assert fake.moved_to == QPoint(899 - TOOLBAR_W, 1000 - TOOLBAR_H - MARGIN)

    def test_panel_height_participates_in_the_fit_decision(self):
        """
        工具栏自己放得下、但加上二级面板就放不下时也要翻到上方——
        否则展开面板会被屏幕底边截断。
        """
        rect = QRectF(500, 900, 400, 60)
        assert _place(rect, panel_extra=0)._toolbar_below_selection is True
        assert _place(rect, panel_extra=200)._toolbar_below_selection is False

    def test_flipping_above_is_clamped_to_the_top_of_the_screen(self):
        # 选区几乎占满整屏：上方也放不下，y 会算成负数，必须被钳到 0
        fake = _place(QRectF(500, 5, 400, 1070))
        assert fake.moved_to.y() == SCREEN.top()


class TestScreenClamping:

    def test_left_edge_selection_clamps_x_to_the_screen(self):
        # right()=49 → x=49-200=-151
        fake = _place(QRectF(0, 300, 50, 50))
        assert fake.moved_to.x() == SCREEN.left()

    def test_right_edge_selection_clamps_x_inside_the_screen(self):
        # right()=1999 → x=1799，超过 1919-200=1719
        fake = _place(QRectF(1900, 300, 100, 50))
        assert fake.moved_to.x() == SCREEN.right() - TOOLBAR_W

    def test_secondary_screen_geometry_is_respected(self):
        """副屏的坐标原点不是 0，钳制必须按该屏的 geometry 来"""
        right_screen = QRect(1920, 0, 1920, 1080)
        fake = _place(QRectF(3800, 300, 100, 50), screen_rect=right_screen)
        assert fake.moved_to.x() == right_screen.right() - TOOLBAR_W
        assert fake.moved_to.x() >= right_screen.left()


class TestManualPositioning:

    def test_manually_dragged_toolbar_is_never_repositioned(self):
        fake = _place(QRectF(500, 300, 400, 200), manual=True)
        assert fake.moved_to is None
        assert fake._toolbar_below_selection is None


class TestCoordinateConversion:

    def test_parent_widget_maps_scene_coordinates_to_global(self):
        parent_widget = SimpleNamespace(
            mapToGlobal=lambda p: QPoint(p.x() + 1000, p.y() + 500))
        fake = _FakeToolbar()
        Toolbar.position_near_rect(fake, QRectF(0, 0, 100, 100), parent_widget)
        # tl(0,0)→(1000,500)，br(100,100)→(1100,600)；QRect 右下即 1100/600
        assert fake.moved_to == QPoint(1100 - TOOLBAR_W, 600 + MARGIN)

    def test_final_position_is_mapped_into_the_parent_window(self):
        parent = SimpleNamespace(
            mapFromGlobal=lambda p: QPoint(p.x() - 100, p.y() - 100))
        fake = _place(QRectF(500, 300, 400, 200), parent=parent)
        assert fake.moved_to == QPoint(899 - TOOLBAR_W - 100, 499 + MARGIN - 100)


class TestMaxPanelHeight:

    def _panel(self, height):
        return SimpleNamespace(sizeHint=lambda: SimpleNamespace(height=lambda: height))

    def test_no_panels_reserve_no_space(self):
        assert Toolbar._get_max_panel_height(SimpleNamespace()) == 0

    def test_single_panel_reserves_its_height_plus_the_gap(self):
        fake = SimpleNamespace(paint_panel=self._panel(120))
        assert Toolbar._get_max_panel_height(fake) == 125

    def test_tallest_panel_wins(self):
        fake = SimpleNamespace(
            paint_panel=self._panel(80),
            shape_panel=self._panel(200),
            arrow_panel=self._panel(150),
        )
        assert Toolbar._get_max_panel_height(fake) == 205

    def test_none_panels_are_skipped(self):
        fake = SimpleNamespace(paint_panel=None, text_panel=self._panel(60))
        assert Toolbar._get_max_panel_height(fake) == 65


class TestScreenByCenter:

    def _fake_qapplication(self, monkeypatch, at_result):
        primary = SimpleNamespace(name="primary")
        seen = []

        class _FakeQApplication:
            @staticmethod
            def screenAt(point):
                seen.append(point)
                return at_result

            @staticmethod
            def primaryScreen():
                return primary

        monkeypatch.setattr(toolbar_mod, "QApplication", _FakeQApplication)
        return primary, seen

    def test_screen_is_chosen_by_the_selection_centre(self, monkeypatch):
        target = SimpleNamespace(name="target")
        _, seen = self._fake_qapplication(monkeypatch, target)
        rect = QRect(100, 200, 400, 200)
        assert Toolbar._get_screen_by_center(SimpleNamespace(), rect) is target
        assert seen == [rect.center()]

    def test_falls_back_to_the_primary_screen(self, monkeypatch):
        primary, _ = self._fake_qapplication(monkeypatch, None)
        got = Toolbar._get_screen_by_center(SimpleNamespace(), QRect(0, 0, 10, 10))
        assert got is primary


class _FakeButton:
    def __init__(self):
        self.checked = None

    def setChecked(self, value):
        self.checked = value


class _Recorder:
    def __init__(self):
        self.calls = []

    def emit(self, *args):
        self.calls.append(args)

    def __call__(self, *args):
        self.calls.append(args)


def _make_toolbar_state(current_tool=None, tools=("pen", "rect", "text")):
    hide_all = _Recorder()
    show_panel = _Recorder()
    state = SimpleNamespace(
        current_tool=current_tool,
        tool_buttons={tool: _FakeButton() for tool in tools},
        tool_changed=_Recorder(),
        _hide_all_panels=hide_all,
        _show_panel_for_tool=show_panel,
    )
    return state, hide_all, show_panel


class TestToolClickStateMachine:

    def test_clicking_a_new_tool_selects_it_exclusively(self):
        state, _, show_panel = _make_toolbar_state()
        Toolbar._on_tool_clicked(state, "pen")
        assert state.current_tool == "pen"
        assert state.tool_buttons["pen"].checked is True
        assert state.tool_buttons["rect"].checked is False
        assert state.tool_buttons["text"].checked is False
        assert state.tool_changed.calls == [("pen",)]
        assert show_panel.calls == [("pen",)]

    def test_clicking_the_active_tool_again_cancels_it(self):
        state, hide_all, show_panel = _make_toolbar_state(current_tool="pen")
        Toolbar._on_tool_clicked(state, "pen")
        assert state.current_tool is None
        assert all(btn.checked is False for btn in state.tool_buttons.values())
        # 退出绘制模式，回到光标
        assert state.tool_changed.calls == [("cursor",)]
        assert hide_all.calls == [()]
        assert show_panel.calls == []

    def test_switching_tools_unchecks_the_previous_one(self):
        state, _, _ = _make_toolbar_state(current_tool="pen")
        state.tool_buttons["pen"].checked = True
        Toolbar._on_tool_clicked(state, "text")
        assert state.current_tool == "text"
        assert state.tool_buttons["pen"].checked is False
        assert state.tool_buttons["text"].checked is True

    def test_shape_tool_without_a_panel_does_not_raise(self):
        """rect/ellipse 会尝试同步线条样式，缺 shape_panel 时必须安静跳过"""
        state, _, show_panel = _make_toolbar_state()
        Toolbar._on_tool_clicked(state, "rect")
        assert state.current_tool == "rect"
        assert show_panel.calls == [("rect",)]


class TestResetSessionState:

    def test_new_session_clears_selection_and_hides_the_toolbar(self):
        state, hide_all, _ = _make_toolbar_state(current_tool="pen")
        state.tool_buttons["pen"].checked = True
        state._manual_positioned = True
        state._dragging = True
        manual_mode = _Recorder()
        state.drag_handle = SimpleNamespace(set_manual_mode=manual_mode)
        state.hide = _Recorder()

        Toolbar.reset_session_state(state)

        assert state.current_tool is None
        assert all(btn.checked is False for btn in state.tool_buttons.values())
        assert hide_all.calls == [()]
        assert state._manual_positioned is False
        assert state._dragging is False
        assert manual_mode.calls == [(False,)]
        assert state.hide.calls == [()]
