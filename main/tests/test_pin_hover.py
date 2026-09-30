# -*- coding: utf-8 -*-
"""
钉图悬停控件显隐测试（pin/pin_hover.py）

PinHoverControls 是右上角按钮和工具栏显隐的唯一决策点。这里用假钉图驱动它，
按 pin_hover.py 开头那张规则表逐条验证，重点是：
- 自动模式下手动关掉工具栏，只要还在这次悬停里，鼠标再怎么动也不会被重新弹出
- 离开后宽限期结束，手动选择清空，下次移入照常自动弹出
- 右键菜单开着算悬停，编辑中不结束会话，缩略图模式一律隐藏
"""
from types import SimpleNamespace

import pytest
from PySide6.QtCore import QObject

from pin import pin_hover
from pin.pin_hover import PinHoverControls


class FakeToolbar:
    def __init__(self):
        self.visible = False

    def isVisible(self):
        return self.visible


class FakeButtons:
    def __init__(self):
        self.close = False
        self.toolbar = False

    def set_visible(self, close, toolbar):
        self.close = close
        self.toolbar = toolbar


class FakeConfig:
    def __init__(self, auto, hover_buttons=True):
        self.auto = auto
        self.hover_buttons = hover_buttons

    def get_pin_auto_toolbar(self):
        return self.auto

    def get_app_setting(self, key, default=None):
        return {"pin_hover_buttons": self.hover_buttons}.get(key, default)


class FakePin(QObject):
    """只实现 PinHoverControls 读写的那部分 PinWindow 接口"""

    def __init__(self, auto, hover_buttons=True):
        super().__init__()
        self.config_manager = FakeConfig(auto, hover_buttons)
        self._is_closed = False
        self._is_editing = False
        self._thumbnail_mode = False
        self.toolbar = None
        self._control_buttons = FakeButtons()

    def _show_toolbar(self):
        if self.toolbar is None:
            self.toolbar = FakeToolbar()
        self.toolbar.visible = True

    def _hide_toolbar(self):
        self.toolbar.visible = False

    @property
    def toolbar_shown(self):
        return self.toolbar is not None and self.toolbar.visible


def _expire(timer):
    """让单次定时器到期：Qt 在发 timeout 之前已经把它停了"""
    assert timer.isActive()
    timer.stop()
    timer.timeout.emit()


@pytest.fixture
def make(qapp):
    created = []

    def _make(auto=True, hover_buttons=True):
        pin = FakePin(auto, hover_buttons)
        ctrl = PinHoverControls(pin)
        created.append(ctrl)
        return pin, ctrl

    yield _make
    for ctrl in created:
        ctrl.stop()


@pytest.fixture
def mouse_over(monkeypatch):
    """决定 QApplication.widgetAt 返回谁；传 None 表示鼠标下不是任何窗口"""
    def _set(pin):
        under = None if pin is None else SimpleNamespace(window=lambda: pin)
        monkeypatch.setattr(pin_hover.QApplication, "widgetAt", staticmethod(lambda _pos: under))
    return _set


def _leave(ctrl):
    ctrl.set_pin_hovered(False)


# ============================================================================
# 自动模式
# ============================================================================

class TestAutoMode:

    def test_hovering_shows_the_toolbar(self, make):
        pin, ctrl = make(auto=True)
        ctrl.set_pin_hovered(True)
        assert pin.toolbar_shown

    def test_toolbar_stays_through_the_grace_period_then_hides(self, make):
        pin, ctrl = make(auto=True)
        ctrl.set_pin_hovered(True)
        _leave(ctrl)
        assert pin.toolbar_shown

        _expire(ctrl._session_timer)
        assert not pin.toolbar_shown

    def test_moving_onto_the_toolbar_keeps_it(self, make):
        pin, ctrl = make(auto=True)
        ctrl.set_pin_hovered(True)
        _leave(ctrl)
        ctrl.set_toolbar_hovered(True)
        assert not ctrl._session_timer.isActive()
        assert pin.toolbar_shown

    def test_manual_close_is_not_undone_by_mouse_moves(self, make):
        """鼠标移动不会把手动关掉的工具栏重新弹出来"""
        pin, ctrl = make(auto=True)
        ctrl.set_pin_hovered(True)
        ctrl.toggle_toolbar()
        assert not pin.toolbar_shown

        for _ in range(3):
            ctrl.set_pin_hovered(True)
        ctrl.sync()
        assert not pin.toolbar_shown

    def test_manual_close_lasts_only_for_this_hover(self, make):
        pin, ctrl = make(auto=True)
        ctrl.set_pin_hovered(True)
        ctrl.toggle_toolbar()
        _leave(ctrl)
        _expire(ctrl._session_timer)

        ctrl.set_pin_hovered(True)
        assert pin.toolbar_shown

    def test_coming_back_within_the_grace_period_keeps_the_manual_close(self, make):
        pin, ctrl = make(auto=True)
        ctrl.set_pin_hovered(True)
        ctrl.toggle_toolbar()
        _leave(ctrl)
        ctrl.set_pin_hovered(True)
        assert not pin.toolbar_shown

    def test_reopening_after_a_manual_close(self, make):
        pin, ctrl = make(auto=True)
        ctrl.set_pin_hovered(True)
        ctrl.toggle_toolbar()
        ctrl.toggle_toolbar()
        assert pin.toolbar_shown

    def test_toggle_without_hovering_still_ends_after_the_grace_period(self, make):
        """快捷键在鼠标不在钉图上时打开的工具栏，和悬停弹出的一样会收起"""
        pin, ctrl = make(auto=True)
        ctrl.toggle_toolbar()
        assert pin.toolbar_shown

        _expire(ctrl._session_timer)
        assert not pin.toolbar_shown


# ============================================================================
# 手动模式
# ============================================================================

class TestManualMode:

    def test_hovering_does_not_show_the_toolbar(self, make):
        pin, ctrl = make(auto=False)
        ctrl.set_pin_hovered(True)
        assert not pin.toolbar_shown

    def test_opened_toolbar_survives_leaving(self, make):
        pin, ctrl = make(auto=False)
        ctrl.set_pin_hovered(True)
        ctrl.toggle_toolbar()
        _leave(ctrl)
        _expire(ctrl._session_timer)
        assert pin.toolbar_shown

    def test_closed_toolbar_stays_closed(self, make):
        pin, ctrl = make(auto=False)
        ctrl.set_pin_hovered(True)
        ctrl.toggle_toolbar()
        ctrl.toggle_toolbar()
        _leave(ctrl)
        _expire(ctrl._session_timer)
        ctrl.set_pin_hovered(True)
        assert not pin.toolbar_shown


# ============================================================================
# 编辑、右键菜单、缩略图、关闭
# ============================================================================

class TestOtherInputs:

    def test_editing_keeps_the_toolbar_past_the_grace_period(self, make):
        pin, ctrl = make(auto=True)
        ctrl.set_pin_hovered(True)
        pin._is_editing = True
        ctrl.on_editing_changed()
        _leave(ctrl)
        _expire(ctrl._session_timer)
        assert pin.toolbar_shown

        pin._is_editing = False
        ctrl.on_editing_changed()
        assert pin.toolbar_shown
        _expire(ctrl._session_timer)
        assert not pin.toolbar_shown

    def test_open_menu_counts_as_hovering(self, make, mouse_over):
        pin, ctrl = make(auto=True)
        ctrl.set_pin_hovered(True)
        ctrl.set_menu_open(True)
        _leave(ctrl)
        assert not ctrl._session_timer.isActive()
        assert pin.toolbar_shown

    def test_closing_the_menu_away_from_the_pin_starts_the_grace_period(self, make, mouse_over):
        pin, ctrl = make(auto=True)
        ctrl.set_pin_hovered(True)
        ctrl.set_menu_open(True)
        _leave(ctrl)
        mouse_over(None)
        ctrl.set_menu_open(False)
        _expire(ctrl._session_timer)
        assert not pin.toolbar_shown

    def test_closing_the_menu_over_the_pin_keeps_hovering(self, make, mouse_over):
        """菜单弹出时钉图收到过 Leave，关菜单后要按鼠标实际位置补回悬停"""
        pin, ctrl = make(auto=True)
        ctrl.set_pin_hovered(True)
        ctrl.set_menu_open(True)
        _leave(ctrl)
        mouse_over(pin)
        ctrl.set_menu_open(False)
        assert not ctrl._session_timer.isActive()
        assert pin.toolbar_shown

    def test_leave_onto_the_pins_own_child_is_still_hovering(self, make, mouse_over):
        pin, ctrl = make(auto=True)
        ctrl.set_pin_hovered(True)
        mouse_over(pin)
        ctrl.recheck_pin_hovered()
        assert not ctrl._session_timer.isActive()

    def test_thumbnail_mode_hides_everything_and_ignores_toggle(self, make):
        pin, ctrl = make(auto=False)
        ctrl.set_pin_hovered(True)
        ctrl.toggle_toolbar()

        pin._thumbnail_mode = True
        ctrl.sync()
        assert not pin.toolbar_shown
        ctrl.toggle_toolbar()
        assert not pin.toolbar_shown

        pin._thumbnail_mode = False
        ctrl.sync()
        assert pin.toolbar_shown

    def test_closed_window_is_left_alone(self, make):
        pin, ctrl = make(auto=True)
        pin._is_closed = True
        ctrl.set_pin_hovered(True)
        assert not pin.toolbar_shown


# ============================================================================
# 右上角按钮
# ============================================================================

class TestButtons:

    def test_close_button_follows_hover_with_a_short_grace(self, make):
        pin, ctrl = make(auto=True)
        ctrl.set_pin_hovered(True)
        assert pin._control_buttons.close

        _leave(ctrl)
        assert pin._control_buttons.close
        _expire(ctrl._buttons_timer)
        assert not pin._control_buttons.close

    @pytest.mark.parametrize("auto, thumbnail, expected", [
        (False, False, True),
        (True, False, False),
        (False, True, False),
    ])
    def test_toolbar_button_only_in_manual_mode_outside_thumbnail(self, make, auto, thumbnail, expected):
        pin, ctrl = make(auto=auto)
        pin._thumbnail_mode = thumbnail
        ctrl.set_pin_hovered(True)
        assert pin._control_buttons.close
        assert pin._control_buttons.toolbar is expected

    def test_setting_off_hides_both_buttons(self, make):
        pin, ctrl = make(auto=False, hover_buttons=False)
        ctrl.set_pin_hovered(True)
        assert not pin._control_buttons.close
        assert not pin._control_buttons.toolbar
