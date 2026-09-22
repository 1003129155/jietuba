# -*- coding: utf-8 -*-
"""
CaptureService 单元测试

覆盖 main/capture/capture_service.py 中的多屏幕截图捕获逻辑。
hdrcapture 需要真实的 DXGI 会话、mss 需要真实的屏幕会话，在无桌面的 CI runner 上
都不可用，因此两条后端路径都用 unittest.mock 模拟。
"""
from unittest.mock import MagicMock, patch

import pytest
from PySide6.QtGui import QImage
from PySide6.QtCore import QRectF

from capture.capture_service import CaptureService


def _make_fake_screenshot(width, height):
    """构造一个符合 mss ScreenShot 接口的假对象（.bgra / .width / .height）"""
    shot = MagicMock()
    shot.width = width
    shot.height = height
    # BGRA，每像素4字节，全部填0（黑色不透明）即可，只关心尺寸和类型转换是否正确
    shot.bgra = bytes(width * height * 4)
    return shot


def _make_fake_mss(monitor, shot):
    """构造 mss.mss() 上下文管理器的 mock，返回 (patcher 用的 mock, 假 sct)"""
    fake_sct = MagicMock()
    fake_sct.monitors = [monitor]
    fake_sct.grab.return_value = shot
    mock_mss = MagicMock()
    mock_mss.return_value.__enter__.return_value = fake_sct
    mock_mss.return_value.__exit__.return_value = False
    return mock_mss, fake_sct


def _make_fake_hdr_capture(rect, width, height):
    """构造符合 hdrcapture.Capture 接口的假会话（.monitors / .grab）"""
    monitor = MagicMock()
    monitor.rect = rect
    frame = MagicMock()
    frame.width = width
    frame.height = height
    frame.bgra = bytes(width * height * 4)

    capture = MagicMock()
    capture.monitors = [monitor]
    capture.grab.return_value = frame
    return capture


@pytest.fixture
def no_hdr():
    """强制走 mss 后端：模拟 hdrcapture 缺失或会话不可用。"""
    with patch("capture.capture_service._HdrSession.acquire", return_value=None):
        yield


class TestMssBackend:
    """hdrcapture 不可用时的回落路径。"""

    def test_returns_qimage_and_qrectf(self, qapp, no_hdr):
        """capture_all_screens 应返回 (QImage, QRectF) 二元组"""
        service = CaptureService()
        mock_mss, _ = _make_fake_mss(
            {"left": 0, "top": 0, "width": 1920, "height": 1080},
            _make_fake_screenshot(1920, 1080),
        )

        with patch("capture.capture_service.mss.mss", mock_mss):
            image, rect = service.capture_all_screens()

        assert isinstance(image, QImage)
        assert isinstance(rect, QRectF)

    def test_image_dimensions_match_monitor(self, qapp, no_hdr):
        """返回的 QImage 尺寸应与虚拟桌面 monitor[0] 一致"""
        service = CaptureService()
        mock_mss, _ = _make_fake_mss(
            {"left": 0, "top": 0, "width": 800, "height": 600},
            _make_fake_screenshot(800, 600),
        )

        with patch("capture.capture_service.mss.mss", mock_mss):
            image, _rect = service.capture_all_screens()

        assert image.width() == 800
        assert image.height() == 600

    def test_rect_uses_virtual_desktop_geometry(self, qapp, no_hdr):
        """返回的 QRectF 应反映多屏偏移（负坐标场景，如主屏左侧的副屏）"""
        service = CaptureService()
        mock_mss, _ = _make_fake_mss(
            {"left": -1920, "top": 0, "width": 3840, "height": 1080},
            _make_fake_screenshot(3840, 1080),
        )

        with patch("capture.capture_service.mss.mss", mock_mss):
            _image, rect = service.capture_all_screens()

        assert rect.x() == -1920
        assert rect.y() == 0
        assert rect.width() == 3840
        assert rect.height() == 1080

    def test_grab_called_with_all_monitors_region(self, qapp, no_hdr):
        """应该用 monitors[0]（合并虚拟桌面区域）调用 sct.grab，而不是单个物理屏幕"""
        service = CaptureService()
        monitor = {"left": 0, "top": 0, "width": 2560, "height": 1440}
        mock_mss, fake_sct = _make_fake_mss(monitor, _make_fake_screenshot(2560, 1440))
        fake_sct.monitors = [monitor, {"left": 0, "top": 0, "width": 2560, "height": 1440}]

        with patch("capture.capture_service.mss.mss", mock_mss):
            service.capture_all_screens()

        fake_sct.grab.assert_called_once_with(monitor)

    def test_returned_image_is_independent_copy(self, qapp, no_hdr):
        """QImage 必须是拷贝（.copy()），不能持有对已失效 mss 缓冲区的引用"""
        service = CaptureService()
        shot = _make_fake_screenshot(100, 100)
        mock_mss, _ = _make_fake_mss({"left": 0, "top": 0, "width": 100, "height": 100}, shot)

        with patch("capture.capture_service.mss.mss", mock_mss):
            image, _rect = service.capture_all_screens()

        # copy() 产生的 QImage 不应与原始 bytes 缓冲区共享内存；
        # 验证方式：即使原始 bytes 对象被销毁，image 仍可安全访问像素数据
        del shot
        _ = image.constBits()
        assert image.width() == 100


class TestHdrBackend:
    """hdrcapture 可用时的首选路径。"""

    def test_prefers_hdr_over_mss(self, qapp):
        """会话可用时必须走 hdrcapture，完全不碰 mss"""
        service = CaptureService()
        capture = _make_fake_hdr_capture((0, 0, 2560, 1440), 2560, 1440)
        mock_mss, _ = _make_fake_mss({"left": 0, "top": 0, "width": 1, "height": 1},
                                     _make_fake_screenshot(1, 1))

        with patch("capture.capture_service._HdrSession.acquire", return_value=capture), \
             patch("capture.capture_service.mss.mss", mock_mss):
            image, rect = service.capture_all_screens()

        mock_mss.assert_not_called()
        assert image.width() == 2560
        assert rect.width() == 2560

    def test_rect_comes_from_monitor_tuple(self, qapp):
        """monitor.rect 是 (x, y, w, h) 元组，负坐标需原样传给 QRectF"""
        service = CaptureService()
        capture = _make_fake_hdr_capture((-1920, -200, 3840, 1080), 3840, 1080)

        with patch("capture.capture_service._HdrSession.acquire", return_value=capture):
            _image, rect = service.capture_all_screens()

        assert (rect.x(), rect.y(), rect.width(), rect.height()) == (-1920, -200, 3840, 1080)

    def test_grabs_virtual_desktop_selector(self, qapp):
        """必须用 monitors[0]（虚拟桌面）而不是某块物理屏"""
        service = CaptureService()
        capture = _make_fake_hdr_capture((0, 0, 800, 600), 800, 600)
        physical = MagicMock()
        physical.rect = (0, 0, 800, 600)
        capture.monitors = [capture.monitors[0], physical]

        with patch("capture.capture_service._HdrSession.acquire", return_value=capture):
            service.capture_all_screens()

        assert capture.grab.call_args.args[0] is capture.monitors[0]

    def test_falls_back_to_mss_when_grab_fails(self, qapp):
        """grab 抛错（如显示器休眠等不到首帧）时必须回落 mss，而不是把异常抛给调用方"""
        service = CaptureService()
        capture = _make_fake_hdr_capture((0, 0, 2560, 1440), 2560, 1440)
        capture.grab.side_effect = RuntimeError("did not deliver an initial frame")
        mock_mss, _ = _make_fake_mss({"left": 0, "top": 0, "width": 640, "height": 480},
                                     _make_fake_screenshot(640, 480))

        with patch("capture.capture_service._HdrSession.acquire", return_value=capture), \
             patch("capture.capture_service.mss.mss", mock_mss):
            image, rect = service.capture_all_screens()

        assert image.width() == 640
        assert rect.width() == 640

    def test_returned_image_is_independent_copy(self, qapp):
        """QImage 不持有 frame.bgra 的引用，frame 回收后像素仍可访问"""
        service = CaptureService()
        capture = _make_fake_hdr_capture((0, 0, 100, 100), 100, 100)

        with patch("capture.capture_service._HdrSession.acquire", return_value=capture):
            image, _rect = service.capture_all_screens()

        capture.grab.return_value = None
        _ = image.constBits()
        assert image.width() == 100
