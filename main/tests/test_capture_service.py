# -*- coding: utf-8 -*-
"""
CaptureService 单元测试

覆盖 main/capture/capture_service.py 中的多屏幕截图捕获逻辑。
mss 依赖真实的操作系统屏幕会话，在无桌面的 CI runner 上不可用，
因此用 unittest.mock 模拟 mss.mss() 上下文管理器和其返回的截图对象。
"""
from unittest.mock import MagicMock, patch

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
