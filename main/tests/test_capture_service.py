# -*- coding: utf-8 -*-
"""
CaptureService 单元测试

覆盖 main/capture/capture_service.py 中的多屏幕截图捕获逻辑，
以及 main/capture/system_cursor.py 把鼠标指针画进截图的部分。
mss 依赖真实的操作系统屏幕会话，在无桌面的 CI runner 上不可用，
因此用 unittest.mock 模拟 mss.mss() 上下文管理器和其返回的截图对象。
指针用系统自带的箭头和 I 形光标句柄，不依赖当前真实的鼠标状态。
"""
import ctypes
from ctypes import wintypes
from types import SimpleNamespace
from unittest.mock import MagicMock, patch

import pytest
from PySide6.QtGui import QColor, QImage
from PySide6.QtCore import QRectF

from capture.capture_service import CaptureService
from capture.system_cursor import SystemCursor, _icon_geometry, _user32 as _cursor_user32

_IDC_ARROW = 32512
_IDC_IBEAM = 32513


def _make_fake_screenshot(width, height):
    """构造一个符合 mss ScreenShot 接口的假对象（.bgra / .width / .height）"""
    shot = MagicMock()
    shot.width = width
    shot.height = height
    # BGRA，每像素4字节，全部填0（黑色不透明）即可，只关心尺寸和类型转换是否正确
    shot.bgra = bytes(width * height * 4)
    return shot


class TestCaptureAllScreens:
    def test_returns_qimage_and_qrectf(self, qapp):
        """capture_all_screens 应返回 (QImage, QRectF) 二元组"""
        service = CaptureService()

        fake_monitor = {"left": 0, "top": 0, "width": 1920, "height": 1080}
        fake_shot = _make_fake_screenshot(1920, 1080)

        fake_sct = MagicMock()
        fake_sct.monitors = [fake_monitor]
        fake_sct.grab.return_value = fake_shot

        with patch("capture.capture_service.mss.mss") as mock_mss:
            mock_mss.return_value.__enter__.return_value = fake_sct
            mock_mss.return_value.__exit__.return_value = False

            image, rect = service.capture_all_screens()

        assert isinstance(image, QImage)
        assert isinstance(rect, QRectF)

    def test_image_dimensions_match_monitor(self, qapp):
        """返回的 QImage 尺寸应与虚拟桌面 monitor[0] 一致"""
        service = CaptureService()

        fake_monitor = {"left": 0, "top": 0, "width": 800, "height": 600}
        fake_shot = _make_fake_screenshot(800, 600)

        fake_sct = MagicMock()
        fake_sct.monitors = [fake_monitor]
        fake_sct.grab.return_value = fake_shot

        with patch("capture.capture_service.mss.mss") as mock_mss:
            mock_mss.return_value.__enter__.return_value = fake_sct
            mock_mss.return_value.__exit__.return_value = False

            image, rect = service.capture_all_screens()

        assert image.width() == 800
        assert image.height() == 600

    def test_rect_uses_virtual_desktop_geometry(self, qapp):
        """返回的 QRectF 应反映多屏偏移（负坐标场景，如主屏左侧的副屏）"""
        service = CaptureService()

        # 典型的多屏布局：副屏在主屏左侧，virtual desktop 原点为负
        fake_monitor = {"left": -1920, "top": 0, "width": 3840, "height": 1080}
        fake_shot = _make_fake_screenshot(3840, 1080)

        fake_sct = MagicMock()
        fake_sct.monitors = [fake_monitor]
        fake_sct.grab.return_value = fake_shot

        with patch("capture.capture_service.mss.mss") as mock_mss:
            mock_mss.return_value.__enter__.return_value = fake_sct
            mock_mss.return_value.__exit__.return_value = False

            image, rect = service.capture_all_screens()

        assert rect.x() == -1920
        assert rect.y() == 0
        assert rect.width() == 3840
        assert rect.height() == 1080

    def test_grab_called_with_all_monitors_region(self, qapp):
        """应该用 monitors[0]（合并虚拟桌面区域）调用 sct.grab，而不是单个物理屏幕"""
        service = CaptureService()

        fake_monitor = {"left": 0, "top": 0, "width": 2560, "height": 1440}
        fake_shot = _make_fake_screenshot(2560, 1440)

        fake_sct = MagicMock()
        fake_sct.monitors = [fake_monitor, {"left": 0, "top": 0, "width": 2560, "height": 1440}]
        fake_sct.grab.return_value = fake_shot

        with patch("capture.capture_service.mss.mss") as mock_mss:
            mock_mss.return_value.__enter__.return_value = fake_sct
            mock_mss.return_value.__exit__.return_value = False

            service.capture_all_screens()

        fake_sct.grab.assert_called_once_with(fake_monitor)

    def test_returned_image_is_independent_copy(self, qapp):
        """QImage 必须是拷贝（.copy()），不能持有对已失效 mss 缓冲区的引用"""
        service = CaptureService()

        fake_monitor = {"left": 0, "top": 0, "width": 100, "height": 100}
        fake_shot = _make_fake_screenshot(100, 100)

        fake_sct = MagicMock()
        fake_sct.monitors = [fake_monitor]
        fake_sct.grab.return_value = fake_shot

        with patch("capture.capture_service.mss.mss") as mock_mss:
            mock_mss.return_value.__enter__.return_value = fake_sct
            mock_mss.return_value.__exit__.return_value = False

            image, _ = service.capture_all_screens()

        # copy() 产生的 QImage 不应与原始 bytes 缓冲区共享内存；
        # 验证方式：即使原始 bytes 对象被销毁，image 仍可安全访问像素数据
        del fake_shot
        # 不应抛出异常（若未 copy，底层内存可能已被回收）
        _ = image.constBits()
        assert image.width() == 100


def _capture_with(monitor, cursor):
    fake_sct = MagicMock()
    fake_sct.monitors = [monitor]
    fake_sct.grab.return_value = _make_fake_screenshot(monitor["width"], monitor["height"])

    with patch("capture.capture_service.mss.mss") as mock_mss:
        mock_mss.return_value.__enter__.return_value = fake_sct
        mock_mss.return_value.__exit__.return_value = False
        return CaptureService().capture_all_screens(cursor)


class TestCaptureWithCursor:
    def test_cursor_drawn_relative_to_virtual_desktop_origin(self, qapp):
        cursor = MagicMock()

        image, _ = _capture_with({"left": -1920, "top": 0, "width": 3840, "height": 1080}, cursor)

        cursor.draw_onto.assert_called_once_with(image, -1920, 0)

    def test_cursor_draw_failure_keeps_screenshot(self, qapp):
        cursor = MagicMock()
        cursor.draw_onto.side_effect = RuntimeError("boom")

        image, _ = _capture_with({"left": 0, "top": 0, "width": 64, "height": 64}, cursor)

        assert image.width() == 64

    def test_refresh_background_reuses_session_cursor(self, qapp):
        from ui.selection_info.controller import SelectionInfoController

        cursor = object()
        new_image = QImage(8, 8, QImage.Format.Format_RGB32)
        win = SimpleNamespace(
            _is_closing=False,
            _exclude_from_capture_set=True,
            _capture_cursor=cursor,
            original_image=None,
            scene=SimpleNamespace(background=MagicMock()),
        )
        controller = SimpleNamespace(_parent_window=win)

        with patch("ui.selection_info.controller.CaptureService") as service:
            service.return_value.capture_all_screens.return_value = (new_image, QRectF())
            SelectionInfoController._do_refresh_background(controller)

        service.return_value.capture_all_screens.assert_called_once_with(cursor)
        assert win.original_image is new_image


def _system_cursor(idc, hotspot_x, hotspot_y):
    """用系统自带指针构造快照，热点落在给定的屏幕坐标。"""
    user32 = ctypes.WinDLL("user32")
    user32.LoadCursorW.argtypes = [wintypes.HINSTANCE, ctypes.c_void_p]
    user32.LoadCursorW.restype = wintypes.HANDLE
    shared = user32.LoadCursorW(None, idc)
    if not shared:
        pytest.skip("无法加载系统指针")
    handle = _cursor_user32.CopyIcon(shared)
    width, height, hx, hy = _icon_geometry(handle)
    return SystemCursor(handle, hotspot_x - hx, hotspot_y - hy, width, height)


def _filled(color, size=200):
    image = QImage(size, size, QImage.Format.Format_RGB32)
    image.fill(color)
    return image


def _changed(image, color):
    return {(x, y): image.pixel(x, y)
            for y in range(image.height()) for x in range(image.width())
            if image.pixel(x, y) != color.rgb()}


class TestSystemCursorDrawOnto:
    def test_arrow_tip_lands_on_hotspot(self, qapp):
        white = QColor(255, 255, 255)
        image = _filled(white)
        cursor = _system_cursor(_IDC_ARROW, 1100, 600)

        cursor.draw_onto(image, 1000, 500)

        changed = _changed(image, white)
        assert (100, 100) in changed
        left, top = 100 - (1100 - cursor.x), 100 - (600 - cursor.y)
        assert all(left <= x < left + cursor.width and top <= y < top + cursor.height
                   for x, y in changed)

    @pytest.mark.parametrize("background, expected", [
        (QColor(255, 255, 255), 0xFF000000),
        (QColor(0, 0, 0), 0xFFFFFFFF),
    ])
    def test_ibeam_inverts_background(self, qapp, background, expected):
        image = _filled(background)

        _system_cursor(_IDC_IBEAM, 100, 100).draw_onto(image, 0, 0)

        changed = _changed(image, background)
        assert changed
        assert set(changed.values()) == {expected}

    def test_monochrome_cursor_keeps_image_opaque(self, qapp):
        image = _filled(QColor(128, 128, 128))

        _system_cursor(_IDC_IBEAM, 100, 100).draw_onto(image, 0, 0)

        assert all(image.pixel(x, y) >> 24 == 0xFF
                   for y in range(image.height()) for x in range(image.width()))

    def test_cursor_clipped_at_image_edges(self, qapp):
        white = QColor(255, 255, 255)
        image = _filled(white, size=50)
        cursor = _system_cursor(_IDC_ARROW, 0, 0)

        cursor.draw_onto(image, 2, 2)
        assert _changed(image, white)
        assert all(x < cursor.width and y < cursor.height for x, y in _changed(image, white))

        outside = _filled(white, size=50)
        cursor.draw_onto(outside, 500, 500)
        assert not _changed(outside, white)
