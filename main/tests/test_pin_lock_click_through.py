# -*- coding: utf-8 -*-
"""
钉图锁定与鼠标穿透（pin/pin_window.py、pin/pin_click_through_bar.py）

用真实 PinWindow 跑（offscreen 平台），只把两处外部依赖换掉：OCR 初始化，以及
切换 WS_EX_TRANSPARENT 的 Win32 调用——offscreen 下没有真窗口句柄，这里改为记录
每次调用，用来确认穿透样式在该打开、该关闭、该补回的时候都调到了。

覆盖的要点：
- 锁定：描边换成锁定色（描边关着也显示），拖动、滚轮缩放、鼠标手势和按键关闭都被拦下，
  右键菜单的「关闭」照常生效
- 穿透：不透明度降到穿透值、工具条出现并贴着钉图走、隐藏/显示跟着钉图、
  退出后恢复原来的不透明度；工具条上的滑块、拖动手柄、退出按钮都能用
"""
import pytest
from PySide6.QtCore import QPoint, Qt
from PySide6.QtGui import QImage
from PySide6.QtTest import QTest

import pin.pin_window as pin_window_module
from pin.pin_border_overlay import LOCKED_BORDER_COLOR
from pin.pin_ocr_manager import PinOCRManager
from pin.pin_window import PinWindow
from settings.tool_settings import ToolSettingsManager

CLICK_THROUGH_OPACITY = ToolSettingsManager.APP_DEFAULT_SETTINGS["pin_click_through_opacity"]


@pytest.fixture
def click_through_calls(monkeypatch):
    calls = []
    monkeypatch.setattr(
        pin_window_module, "set_window_click_through",
        lambda hwnd, enabled: calls.append(enabled) or True,
    )
    return calls


@pytest.fixture
def make_pin(qapp, tmp_settings, monkeypatch, click_through_calls):
    monkeypatch.setattr(PinOCRManager, "init_now", lambda self, force=False: False)
    config = ToolSettingsManager(tmp_settings)
    created = []

    def _make(pos=QPoint(200, 200), border=True, **settings):
        config.set_app_setting("pin_auto_border", border)
        for key, value in settings.items():
            config.set_app_setting(key, value)
        image = QImage(200, 120, QImage.Format.Format_RGB32)
        image.fill(0xFF336699)
        pin = PinWindow(image, pos, config)
        created.append(pin)
        return pin

    yield _make
    for pin in created:
        pin.close_window()


def _hint(pin):
    return pin._zoom_label.text() if pin._zoom_label.isVisible() else ""


# ============================================================================
# 默认不透明度
# ============================================================================

class TestDefaultOpacity:

    def test_new_pins_start_at_the_configured_opacity(self, make_pin):
        pin = make_pin(pin_default_opacity=0.6)
        assert pin.windowOpacity() == pytest.approx(0.6, abs=0.01)

    def test_a_too_low_setting_is_raised_to_the_floor(self, make_pin):
        """配置文件里写个 0.05 也不能让钉图几乎看不见"""
        pin = make_pin(pin_default_opacity=0.05)
        assert pin.windowOpacity() == pytest.approx(pin_window_module.MIN_OPACITY, abs=0.01)


# ============================================================================
# 锁定
# ============================================================================

class TestLock:

    @pytest.mark.parametrize("border", [True, False])
    def test_locking_shows_the_lock_colour_and_unlocking_restores_the_border(self, make_pin, border):
        pin = make_pin(border=border)
        pin.set_locked(True)
        assert pin.border_overlay is not None and not pin.border_overlay.isHidden()
        assert pin.border_overlay.border_color == LOCKED_BORDER_COLOR
        assert _hint(pin) == "Locked, right-click to unlock"

        pin.set_locked(False)
        assert _hint(pin) == "Unlocked"
        if border:
            assert not pin.border_overlay.isHidden()
            assert pin.border_overlay.border_color == pin.border_color
        else:
            assert pin.border_overlay.isHidden()

    def test_toggling_the_border_while_locked_keeps_the_lock_frame(self, make_pin):
        pin = make_pin(border=True)
        pin.set_locked(True)
        pin.toggle_border_effect()
        assert pin.border_enabled is False
        assert not pin.border_overlay.isHidden()

        pin.set_locked(False)
        assert pin.border_overlay.isHidden()

    def test_drag_does_not_move_a_locked_pin(self, make_pin, qapp):
        pin = make_pin()
        pin.set_locked(True)
        start = pin.pos()
        pin.start_window_drag(QPoint(250, 250))
        pin.update_window_drag(QPoint(400, 380))
        pin.end_window_drag()
        assert pin.pos() == start
        assert _hint(pin) == "Locked, right-click to unlock"

    @pytest.mark.parametrize("action", ["close", "reset", "thumbnail"])
    def test_mouse_gestures_that_move_resize_or_close_are_blocked(self, make_pin, action):
        pin = make_pin()
        pin.set_locked(True)
        geometry = pin.geometry()
        pin._run_mouse_action(action)
        assert not pin._is_closed
        assert not pin._thumbnail_mode
        assert pin.geometry() == geometry

    def test_hover_close_button_and_keys_do_not_close_but_the_menu_does(self, make_pin):
        pin = make_pin()
        pin.set_locked(True)
        pin.close_button.click()
        pin.close_unless_locked()
        assert not pin._is_closed

        pin.close_window()       # 右键菜单「关闭」连的是它
        assert pin._is_closed

    def test_an_unlocked_pin_still_closes_from_the_button(self, make_pin):
        pin = make_pin()
        pin.close_button.click()
        assert pin._is_closed

    def test_menu_state_reports_the_lock(self, make_pin, monkeypatch):
        pin = make_pin()
        seen = {}
        monkeypatch.setattr(pin._context_menu, "show", lambda pos, state: seen.update(state))
        pin.toggle_lock()
        pin.show_context_menu(QPoint(0, 0))
        assert seen["locked"] is True and seen["click_through"] is False


class TestLockSettings:
    """锁定时禁止哪几类操作由设置决定，和 PixPin 一样缩放、调大小归在「移动」里"""

    def test_allowing_moves_lets_a_locked_pin_drag_and_zoom(self, make_pin, qapp):
        pin = make_pin(pin_lock_block_move=False)
        pin.set_locked(True)
        start = pin.pos()
        pin.start_window_drag(QPoint(250, 250))
        pin.update_window_drag(QPoint(300, 270))
        pin.end_window_drag()
        assert pin.pos() == start + QPoint(50, 20)

        pin._adjust_mouse_value("zoom", 120)
        assert pin.scale_factor == pytest.approx(1.1)

    def test_allowing_mouse_close_lets_the_close_button_work(self, make_pin):
        pin = make_pin(pin_lock_block_click_close=False)
        pin.set_locked(True)
        pin.close_unless_locked("key_close")
        assert not pin._is_closed
        pin.close_button.click()
        assert pin._is_closed

    def test_allowing_key_close_lets_esc_work(self, make_pin):
        pin = make_pin(pin_lock_block_key_close=False)
        pin.set_locked(True)
        pin.close_button.click()
        assert not pin._is_closed
        pin.close_unless_locked("key_close")
        assert pin._is_closed

    def test_hints_can_be_turned_off(self, make_pin, qapp):
        pin = make_pin(pin_lock_hint=False)
        pin.set_locked(True)
        assert _hint(pin) == ""
        pin.start_window_drag(QPoint(250, 250))
        pin.update_window_drag(QPoint(400, 400))
        assert _hint(pin) == ""
        assert pin.border_overlay.border_color == LOCKED_BORDER_COLOR


# ============================================================================
# 鼠标穿透
# ============================================================================

class TestClickThrough:

    def test_entering_lowers_opacity_shows_the_bar_and_sets_the_style(self, make_pin, click_through_calls):
        pin = make_pin()
        pin.set_click_through(True)
        assert click_through_calls == [True]
        assert pin.windowOpacity() == pytest.approx(CLICK_THROUGH_OPACITY, abs=0.01)
        bar = pin._click_through_bar
        assert bar is not None and bar.isVisible()
        assert bar._slider.value() == round(CLICK_THROUGH_OPACITY * 100)

        pin.set_click_through(False)
        assert click_through_calls == [True, False]
        assert pin.windowOpacity() == pytest.approx(1.0, abs=0.01)
        assert not bar.isVisible()

    def test_an_already_faint_pin_is_not_made_more_opaque(self, make_pin):
        pin = make_pin()
        pin._set_window_opacity(0.3)
        pin.set_click_through(True)
        assert pin.windowOpacity() == pytest.approx(0.3, abs=0.01)
        pin.set_click_through(False)
        assert pin.windowOpacity() == pytest.approx(0.3, abs=0.01)

    def test_the_slider_opacity_is_remembered_for_next_time(self, make_pin):
        pin = make_pin()
        pin.set_click_through(True)
        pin._click_through_bar._slider.setValue(30)
        assert pin.windowOpacity() == pytest.approx(0.3, abs=0.01)

        pin.set_click_through(False)
        pin.set_click_through(True)
        assert pin.windowOpacity() == pytest.approx(0.3, abs=0.01)

    def test_the_exit_button_leaves_click_through(self, make_pin):
        pin = make_pin()
        pin.set_click_through(True)
        pin._click_through_bar._exit_button.click()
        assert pin._click_through is False

    def test_the_bar_sits_above_the_pin_and_follows_it(self, make_pin):
        pin = make_pin(pos=QPoint(200, 200))
        pin.set_click_through(True)
        bar = pin._click_through_bar
        assert bar.geometry().bottom() < pin.frameGeometry().top()
        assert bar.x() == pin.frameGeometry().left()

        pin.move(260, 240)
        assert bar.geometry().bottom() < pin.frameGeometry().top()
        assert bar.x() == pin.frameGeometry().left()

    def test_the_bar_drops_below_a_pin_at_the_top_of_the_screen(self, make_pin):
        pin = make_pin()
        top = pin.screen().geometry().top()
        pin.move(pin.x(), top)
        pin.set_click_through(True)
        assert pin._click_through_bar.y() > pin.frameGeometry().bottom()

    def test_dragging_the_handle_moves_the_pin_unless_locked(self, make_pin):
        pin = make_pin(pos=QPoint(200, 200))
        pin.set_click_through(True)
        handle = pin._click_through_bar._handle

        def drag(dx, dy):
            QTest.mousePress(handle, Qt.MouseButton.LeftButton, pos=QPoint(5, 5))
            QTest.mouseMove(handle, QPoint(5 + dx, 5 + dy))
            QTest.mouseRelease(handle, Qt.MouseButton.LeftButton, pos=QPoint(5 + dx, 5 + dy))

        start = pin.pos()
        drag(50, 20)
        assert pin.pos() == start + QPoint(50, 20)
        assert pin._click_through_bar.x() == pin.frameGeometry().left()

        pin.set_locked(True)
        moved = pin.pos()
        drag(60, 60)
        assert pin.pos() == moved

    def test_the_bar_hides_and_returns_with_the_pin(self, make_pin):
        pin = make_pin()
        pin.set_click_through(True)
        pin.hide()
        assert not pin._click_through_bar.isVisible()
        pin.show()
        assert pin._click_through_bar.isVisible()

    def test_toggling_always_on_top_reapplies_the_style(self, make_pin, click_through_calls):
        """切换置顶会让 Qt 重设窗口标志、冲掉穿透样式，显示时要补回来"""
        pin = make_pin()
        pin.set_click_through(True)
        click_through_calls.clear()
        pin.toggle_stay_on_top()
        assert click_through_calls == [True]
        assert pin._click_through_bar.isVisible()

    def test_click_through_starts_at_the_configured_opacity(self, make_pin):
        pin = make_pin(pin_click_through_opacity=0.3)
        pin.set_click_through(True)
        assert pin.windowOpacity() == pytest.approx(0.3, abs=0.01)
        assert pin._click_through_bar._slider.value() == 30

    def test_saved_settings_reach_open_pins_on_their_next_click_through(self, make_pin):
        pin = make_pin()
        pin.set_click_through(True)
        pin._click_through_bar._slider.setValue(70)
        pin.set_click_through(False)

        pin.config_manager.set_app_setting("pin_click_through_opacity", 0.4)
        pin.refresh_appearance()
        pin.set_click_through(True)
        assert pin.windowOpacity() == pytest.approx(0.4, abs=0.01)

    def test_closing_a_pin_in_click_through_removes_the_bar(self, make_pin):
        pin = make_pin()
        pin.set_click_through(True)
        pin.close_window()
        assert pin._click_through_bar is None
