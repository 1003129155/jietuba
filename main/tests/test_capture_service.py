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
from PySide6.QtCore import QRect, QRectF

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

    def test_screenshot_uses_adaptive_tone_mapping(self, qapp):
        capture = _make_fake_hdr_capture((0, 0, 100, 100), 100, 100)

        with patch("capture.capture_service._HdrSession.acquire", return_value=capture):
            CaptureService("hdr").capture_all_screens()

        assert capture.grab.call_args.kwargs["adaptive"] is True

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


class TestExplicitEngine:
    """指定引擎时只用那一个，失败不回落。"""

    def test_default_engine_comes_from_settings(self):
        from settings.tool_settings import get_tool_settings_manager

        manager = get_tool_settings_manager()
        manager.set_capture_engine("mss")
        try:
            assert CaptureService().engine == "mss"
        finally:
            manager.set_capture_engine("auto")

    def test_mss_engine_never_opens_hdr_session(self, qapp):
        acquire = MagicMock()
        mock_mss, _ = _make_fake_mss({"left": 0, "top": 0, "width": 640, "height": 480},
                                     _make_fake_screenshot(640, 480))

        with patch("capture.capture_service._HdrSession.acquire", acquire), \
             patch("capture.capture_service.mss.mss", mock_mss):
            image, _rect = CaptureService("mss").capture_all_screens()

        acquire.assert_not_called()
        assert image.width() == 640

    def test_hdr_engine_raises_without_session(self, qapp):
        mock_mss = MagicMock()
        with patch("capture.capture_service._HdrSession.acquire", return_value=None), \
             patch("capture.capture_service.mss.mss", mock_mss), \
             pytest.raises(RuntimeError):
            CaptureService("hdr").capture_all_screens()

        mock_mss.assert_not_called()

    def test_hdr_engine_raises_when_grab_fails(self, qapp):
        capture = _make_fake_hdr_capture((0, 0, 100, 100), 100, 100)
        capture.grab.side_effect = RuntimeError("did not deliver an initial frame")
        mock_mss = MagicMock()

        with patch("capture.capture_service._HdrSession.acquire", return_value=capture), \
             patch("capture.capture_service.mss.mss", mock_mss), \
             pytest.raises(RuntimeError, match="initial frame"):
            CaptureService("hdr").capture_all_screens()

        mock_mss.assert_not_called()


def _fake_monitor(rect):
    monitor = MagicMock()
    monitor.rect = rect
    return monitor


def _gradient_frame(width, height):
    """每个像素的 B 通道 = x % 256、G 通道 = y % 256，用来核对裁剪位置。"""
    frame = MagicMock()
    frame.width = width
    frame.height = height
    frame.bgra = bytes(
        channel
        for y in range(height) for x in range(width)
        for channel in (x % 256, y % 256, 0, 255)
    )
    return frame


class TestGrabRegionHdr:
    """长截图用的区域抓取。"""

    def _session(self, monitors, frame):
        session = MagicMock()
        session.monitors = monitors
        session.grab.return_value = frame
        return session

    def test_reads_back_only_the_monitor_containing_region(self, qapp):
        from capture.capture_service import grab_region_hdr

        desktop = _fake_monitor((-64, 0, 128, 32))
        left = _fake_monitor((-64, 0, 64, 32))
        right = _fake_monitor((0, 0, 64, 32))
        session = self._session([desktop, left, right], _gradient_frame(64, 32))

        with patch("capture.capture_service._HdrSession.acquire", return_value=session):
            image = grab_region_hdr(QRect(-54, 5, 20, 10))

        assert session.grab.call_args.args[0] is left
        assert (image.width(), image.height()) == (20, 10)
        # 区域左上角 (-54, 5) 在左屏里是 (10, 5)
        color = image.pixelColor(0, 0)
        assert (color.blue(), color.green()) == (10, 5)

    def test_region_across_monitors_reads_virtual_desktop(self, qapp):
        from capture.capture_service import grab_region_hdr

        desktop = _fake_monitor((-64, 0, 128, 32))
        monitors = [desktop, _fake_monitor((-64, 0, 64, 32)), _fake_monitor((0, 0, 64, 32))]
        session = self._session(monitors, _gradient_frame(128, 32))

        with patch("capture.capture_service._HdrSession.acquire", return_value=session):
            image = grab_region_hdr(QRect(-10, 0, 20, 8))

        assert session.grab.call_args.args[0] is desktop
        color = image.pixelColor(0, 0)
        assert (color.blue(), color.green()) == (54, 0)

    def test_uses_static_tone_mapping(self, qapp):
        """长截图靠相邻帧逐像素一致拼接，映射不能随画面内容变化。"""
        from capture.capture_service import grab_region_hdr

        session = self._session([_fake_monitor((0, 0, 64, 32))], _gradient_frame(64, 32))
        with patch("capture.capture_service._HdrSession.acquire", return_value=session):
            grab_region_hdr(QRect(0, 0, 10, 10))

        assert session.grab.call_args.kwargs["adaptive"] is False

    def test_raises_without_session(self, qapp):
        from capture.capture_service import grab_region_hdr

        with patch("capture.capture_service._HdrSession.acquire", return_value=None), \
             pytest.raises(RuntimeError):
            grab_region_hdr(QRect(0, 0, 10, 10))

    def test_waits_for_dwm_before_grabbing(self, qapp):
        """刚设的截图排除要等下一次 DWM 合成才进 DXGI 帧。"""
        from capture.capture_service import grab_region_hdr

        order = MagicMock()
        session = self._session([_fake_monitor((0, 0, 64, 32))], _gradient_frame(64, 32))
        order.attach_mock(session.grab, "grab")

        with patch("capture.capture_service._HdrSession.acquire", return_value=session), \
             patch("capture.capture_service.ctypes.windll.dwmapi.DwmFlush") as flush:
            order.attach_mock(flush, "flush")
            grab_region_hdr(QRect(0, 0, 10, 10))

        assert [name for name, *_ in order.mock_calls if name in ("flush", "grab")] == ["flush", "grab"]


@pytest.fixture
def fake_hdrcapture():
    """替换 hdrcapture 模块并清空进程级会话状态，测完还原。"""
    from capture.capture_service import _HdrSession

    module = MagicMock()
    with patch("capture.capture_service.hdrcapture", module), \
         patch.multiple(_HdrSession, _capture=None, _owner_thread=None, _failed=False):
        yield module


class TestHdrSessionLifecycle:

    def test_creation_failure_is_remembered(self, fake_hdrcapture):
        from capture.capture_service import _HdrSession

        fake_hdrcapture.Capture.side_effect = OSError("DXGI_ERROR_UNSUPPORTED")
        assert _HdrSession.acquire() is None
        assert _HdrSession.acquire() is None
        assert fake_hdrcapture.Capture.call_count == 1

    def test_warm_up_failure_is_retried_on_first_capture(self, fake_hdrcapture):
        """开机自启时桌面可能还没就绪，预热失败不能把 HDR 永久关掉。"""
        from capture.capture_service import _HdrSession, warm_up_hdr_session

        fake_hdrcapture.Capture.side_effect = [OSError("not ready"), MagicMock()]
        assert warm_up_hdr_session() is False
        assert _HdrSession.acquire() is not None

    def test_warm_up_keeps_session_for_capture(self, fake_hdrcapture):
        from capture.capture_service import _HdrSession, warm_up_hdr_session

        assert warm_up_hdr_session() is True
        assert _HdrSession.acquire() is fake_hdrcapture.Capture.return_value
        assert fake_hdrcapture.Capture.call_count == 1

    def test_switching_to_mss_closes_session(self, fake_hdrcapture):
        from capture.capture_service import _HdrSession, apply_capture_engine

        session = _HdrSession.acquire()
        apply_capture_engine("mss")

        session.close.assert_called_once()
        assert _HdrSession._capture is None

    @pytest.mark.parametrize("engine", ["auto", "hdr"])
    def test_switching_back_retries_failed_session(self, fake_hdrcapture, engine):
        from capture.capture_service import _HdrSession, apply_capture_engine

        fake_hdrcapture.Capture.side_effect = [OSError("unsupported"), MagicMock()]
        assert _HdrSession.acquire() is None
        apply_capture_engine(engine)
        assert _HdrSession.acquire() is not None

    def test_keeping_hdr_does_not_rebuild_session(self, fake_hdrcapture):
        from capture.capture_service import _HdrSession, apply_capture_engine

        session = _HdrSession.acquire()
        apply_capture_engine("auto")

        session.close.assert_not_called()
        assert _HdrSession.acquire() is session
