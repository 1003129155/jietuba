# -*- coding: utf-8 -*-
"""
钉图的过大自动缩小、以鼠标为基准的滚轮缩放、拖边缘调大小（pin/pin_window.py）

用真实 PinWindow（offscreen 平台）。拖边缘的鼠标事件直接发给视图的 viewport，
和真实操作走的是同一个事件过滤器。
"""
import pytest
from PySide6.QtCore import QEvent, QPoint, QPointF, Qt
from PySide6.QtGui import QImage, QMouseEvent
from PySide6.QtWidgets import QApplication

import pin.pin_window as pin_window_module
from pin.pin_ocr_manager import PinOCRManager
from pin.pin_window import AUTO_SHRINK_RATIO, PinWindow
from settings.tool_settings import ToolSettingsManager


@pytest.fixture
def make_pin(qapp, tmp_settings, monkeypatch):
    monkeypatch.setattr(PinOCRManager, "init_now", lambda self, force=False: False)
    monkeypatch.setattr(pin_window_module, "set_window_click_through", lambda hwnd, enabled: True)
    config = ToolSettingsManager(tmp_settings)
    created = []

    def _make(size=(200, 120), pos=QPoint(100, 100), **settings):
        config.set_app_setting("pin_auto_border", False)
        for key, value in settings.items():
            config.set_app_setting(key, value)
        image = QImage(*size, QImage.Format.Format_RGB32)
        image.fill(0xFF336699)
        pin = PinWindow(image, pos, config)
        created.append(pin)
        return pin

    yield _make
    for pin in created:
        pin.close_window()


def _send(pin, kind, pos, button=Qt.MouseButton.NoButton, buttons=Qt.MouseButton.NoButton):
    viewport = pin.view.viewport()
    local = QPointF(pos)
    event = QMouseEvent(kind, local, QPointF(viewport.mapToGlobal(pos)), button, buttons,
                        Qt.KeyboardModifier.NoModifier)
    QApplication.sendEvent(viewport, event)
    return event


def _drag(pin, start, end):
    left = Qt.MouseButton.LeftButton
    _send(pin, QEvent.Type.MouseButtonPress, start, left, left)
    _send(pin, QEvent.Type.MouseMove, end, Qt.MouseButton.NoButton, left)
    _send(pin, QEvent.Type.MouseButtonRelease, end, left, Qt.MouseButton.NoButton)


# ============================================================================
# 过大自动缩小
# ============================================================================

class TestAutoShrink:

    def _screen(self):
        return QApplication.primaryScreen().geometry()

    def test_an_image_larger_than_the_screen_fits_and_is_centered(self, make_pin):
        area = self._screen()
        pin = make_pin(size=(area.width() * 2, area.height() * 3))
        geo = pin.geometry()
        assert geo.height() == pytest.approx(area.height() * AUTO_SHRINK_RATIO, abs=1)
        assert geo.width() / geo.height() == pytest.approx(2 / 3, abs=0.01)
        assert geo.center().x() == pytest.approx(area.center().x(), abs=1)
        assert geo.center().y() == pytest.approx(area.center().y(), abs=1)
        assert pin.scale_factor == pytest.approx(geo.height() / (area.height() * 3), abs=0.001)

    def test_an_image_that_fits_keeps_its_size_and_place(self, make_pin):
        area = self._screen()
        pin = make_pin(size=(area.width() - 10, area.height() - 10), pos=QPoint(5, 5))
        assert pin.geometry().topLeft() == QPoint(5, 5)
        assert pin.scale_factor == 1.0

    def test_the_setting_can_turn_it_off(self, make_pin):
        area = self._screen()
        pin = make_pin(size=(area.width() * 2, 100), pos=QPoint(0, 0), pin_auto_shrink=False)
        assert pin.width() == area.width() * 2
        assert pin.scale_factor == 1.0


# ============================================================================
# 以鼠标为基准缩放
# ============================================================================

def test_mouse_anchored_zoom_keeps_the_point_under_the_cursor(make_pin, monkeypatch):
    pin = make_pin(pin_zoom_anchor="mouse")
    cursor = pin.mapToGlobal(QPoint(150, 30))

    class _Cursor:
        @staticmethod
        def pos():
            return cursor

    monkeypatch.setattr(pin_window_module, "QCursor", _Cursor)
    pin._adjust_mouse_value("zoom", 120)

    local = pin.mapFromGlobal(cursor)
    assert local.x() / pin.width() == pytest.approx(150 / 200, abs=0.01)
    assert local.y() / pin.height() == pytest.approx(30 / 120, abs=0.01)


# ============================================================================
# 拖边缘调大小
# ============================================================================

class TestBorderResize:

    def test_hovering_an_edge_shows_a_resize_cursor_and_the_middle_does_not(self, make_pin):
        pin = make_pin()
        viewport = pin.view.viewport()
        _send(pin, QEvent.Type.MouseMove, QPoint(199, 60))
        assert viewport.cursor().shape() == Qt.CursorShape.SizeHorCursor
        _send(pin, QEvent.Type.MouseMove, QPoint(199, 119))
        assert viewport.cursor().shape() == Qt.CursorShape.SizeFDiagCursor
        _send(pin, QEvent.Type.MouseMove, QPoint(100, 60))
        assert viewport.cursor().shape() == Qt.CursorShape.ArrowCursor

    def test_dragging_the_right_edge_keeps_the_ratio_and_the_top_left(self, make_pin):
        pin = make_pin()
        start = pin.geometry()
        _drag(pin, QPoint(199, 60), QPoint(299, 60))
        geo = pin.geometry()
        assert geo.topLeft() == start.topLeft()
        assert geo.width() == 300
        assert geo.height() == 180
        assert pin.scale_factor == pytest.approx(1.5)

    def test_dragging_the_left_edge_keeps_the_right_edge(self, make_pin):
        pin = make_pin()
        right = pin.geometry().right()
        _drag(pin, QPoint(0, 60), QPoint(100, 60))
        geo = pin.geometry()
        assert geo.right() == right
        assert (geo.width(), geo.height()) == (100, 60)

    def test_a_corner_follows_the_larger_change(self, make_pin):
        """拖右下角往右 100、往下 6：横向变化大，按横向算"""
        pin = make_pin()
        _drag(pin, QPoint(199, 119), QPoint(299, 125))
        assert (pin.width(), pin.height()) == (300, 180)

    def test_resizing_does_not_move_the_window(self, make_pin):
        pin = make_pin()
        start = pin.pos()
        _drag(pin, QPoint(199, 60), QPoint(150, 60))
        assert pin.pos() == start

    def test_the_middle_still_drags_the_window(self, make_pin):
        pin = make_pin()
        start = pin.pos()
        _drag(pin, QPoint(100, 60), QPoint(140, 80))
        assert pin.pos() == start + QPoint(40, 20)
        assert pin.size().width() == 200

    def test_no_resize_when_turned_off(self, make_pin):
        pin = make_pin(pin_border_resize=False)
        _drag(pin, QPoint(199, 60), QPoint(299, 60))
        assert pin.width() == 200

    def test_no_resize_on_a_locked_pin(self, make_pin):
        pin = make_pin()
        pin.set_locked(True)
        _drag(pin, QPoint(199, 60), QPoint(299, 60))
        assert pin.width() == 200

    def test_a_locked_pin_resizes_when_moving_is_allowed(self, make_pin):
        pin = make_pin(pin_lock_block_move=False)
        pin.set_locked(True)
        _drag(pin, QPoint(199, 60), QPoint(299, 60))
        assert pin.width() == 300

    def test_no_resize_in_thumbnail_mode(self, make_pin):
        pin = make_pin()
        pin.toggle_thumbnail_mode()
        size = pin.size()
        _drag(pin, QPoint(size.width() - 1, size.height() // 2),
              QPoint(size.width() + 60, size.height() // 2))
        assert pin.size() == size
