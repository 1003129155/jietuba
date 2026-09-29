# -*- coding: utf-8 -*-
"""
CaptureService 单元测试

覆盖 main/capture/capture_service.py 中的多屏幕截图捕获逻辑，
以及 main/capture/system_cursor.py 把鼠标指针画进截图的部分。
hdrcapture 需要真实的 DXGI 会话、mss 需要真实的屏幕会话，在无桌面的 CI runner 上
都不可用，因此两条后端路径都用 unittest.mock 模拟。
指针用系统自带的箭头和 I 形光标句柄，不依赖当前真实的鼠标状态。
"""
import ctypes
from ctypes import wintypes
from types import SimpleNamespace
from unittest.mock import MagicMock, patch

import pytest
from PySide6.QtGui import QColor, QImage
from PySide6.QtCore import QRect, QRectF

from capture.capture_service import CaptureService
from capture.capture_service import hdr_display_active as _real_hdr_display_active
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


@pytest.fixture(autouse=True)
def hdr_display_on():
    """默认有显示器开着 HDR，auto 引擎走 HDR 路径；个别用例改成 False 测 SDR 屏。"""
    with patch("capture.capture_service.hdr_display_active", return_value=True) as active:
        yield active


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


def _capture_with(monitor, cursor):
    fake_sct = MagicMock()
    fake_sct.monitors = [monitor]
    fake_sct.grab.return_value = _make_fake_screenshot(monitor["width"], monitor["height"])

    with patch("capture.capture_service._HdrSession.acquire", return_value=None), \
         patch("capture.capture_service.mss.mss") as mock_mss:
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


def test_capture_region_uses_absolute_pixels_and_owns_buffer(no_hdr):
    pixels = bytearray([30, 20, 10, 255] * 12)
    shot = MagicMock(width=4, height=3, bgra=pixels)
    with patch("capture.capture_service.mss.mss") as mss_factory:
        context = mss_factory.return_value.__enter__.return_value
        context.grab.return_value = shot
        result = CaptureService().capture_region(QRect(-20, -40, 4, 3))
    context.grab.assert_called_once_with(dict(left=-20, top=-40, width=4, height=3))
    pixels[:] = bytes(len(pixels))
    assert result.size().width() == 4
    assert result.size().height() == 3
    assert result.pixelColor(0, 0).getRgb() == (10, 20, 30, 255)


@pytest.mark.parametrize("draw_fails", [False, True])
def test_capture_region_composes_cursor_relative_to_region(no_hdr, draw_fails):
    cursor = MagicMock()
    if draw_fails:
        cursor.draw_onto.side_effect = RuntimeError("cursor unavailable")
    shot = MagicMock(width=4, height=3, bgra=bytes([30, 20, 10, 255] * 12))
    with patch("capture.capture_service.mss.mss") as mss_factory:
        mss_factory.return_value.__enter__.return_value.grab.return_value = shot
        result = CaptureService().capture_region(QRect(-20, -40, 4, 3), cursor)
    cursor.draw_onto.assert_called_once_with(result, -20, -40)
    assert not result.isNull()
    assert result.pixelColor(0, 0).getRgb() == (10, 20, 30, 255)


@pytest.mark.parametrize("rect", [QRect(), QRect(10, 10, 0, 4), QRect(10, 10, -1, 4)])
def test_capture_region_rejects_empty_or_reversed_bounds(rect):
    with patch("capture.capture_service.mss.mss") as mss_factory:
        with pytest.raises(ValueError, match="positive dimensions"):
            CaptureService().capture_region(rect)
    mss_factory.assert_not_called()


class TestAutoEngineFollowsHdrDisplays:
    """auto 只在有显示器开着 HDR 时用 HDR 引擎：SDR 屏上两者截出来一样，会话只占内存。"""

    def test_auto_uses_mss_without_hdr_display_and_releases_session(self, qapp, hdr_display_on):
        hdr_display_on.return_value = False
        acquire = MagicMock()
        mock_mss, _ = _make_fake_mss({"left": 0, "top": 0, "width": 640, "height": 480},
                                     _make_fake_screenshot(640, 480))

        with patch("capture.capture_service._HdrSession.acquire", acquire), \
             patch("capture.capture_service._HdrSession.release") as release, \
             patch("capture.capture_service.mss.mss", mock_mss):
            image, _rect = CaptureService("auto").capture_all_screens()

        acquire.assert_not_called()
        release.assert_called_once()
        assert image.width() == 640

    def test_auto_region_uses_mss_without_hdr_display(self, qapp, hdr_display_on):
        hdr_display_on.return_value = False
        shot = MagicMock(width=4, height=3, bgra=bytes([30, 20, 10, 255] * 12))
        acquire = MagicMock()

        with patch("capture.capture_service._HdrSession.acquire", acquire), \
             patch("capture.capture_service.mss.mss") as mss_factory:
            mss_factory.return_value.__enter__.return_value.grab.return_value = shot
            image = CaptureService("auto").capture_region(QRect(0, 0, 4, 3))

        acquire.assert_not_called()
        assert image.width() == 4

    def test_hdr_engine_ignores_display_state(self, qapp, hdr_display_on):
        hdr_display_on.return_value = False
        capture = _make_fake_hdr_capture((0, 0, 100, 100), 100, 100)

        with patch("capture.capture_service._HdrSession.acquire", return_value=capture):
            CaptureService("hdr").capture_all_screens()

        capture.grab.assert_called_once()

    @pytest.mark.parametrize("engine, hdr_on, expected", [
        ("auto", True, True), ("auto", False, False),
        ("hdr", False, True), ("mss", True, False),
    ])
    def test_uses_hdr_engine(self, hdr_display_on, engine, hdr_on, expected):
        from capture.capture_service import uses_hdr_engine

        hdr_display_on.return_value = hdr_on
        assert uses_hdr_engine(engine) is expected


class TestHdrDisplayActive:
    """测真实的 hdr_display_active（模块级替身挡不住导入时拿到的原函数），只替换 hdrcapture。"""

    def test_any_display_with_hdr_counts(self):
        displays = [MagicMock(hdr_enabled=False), MagicMock(hdr_enabled=True)]
        with patch("capture.capture_service.hdrcapture") as module:
            module.displays.return_value = displays
            assert _real_hdr_display_active() is True
            module.displays.return_value = displays[:1]
            assert _real_hdr_display_active() is False

    def test_missing_extension_means_no_hdr(self):
        with patch("capture.capture_service.hdrcapture", None):
            assert _real_hdr_display_active() is False

    def test_query_failure_means_no_hdr(self):
        with patch("capture.capture_service.hdrcapture") as module:
            module.displays.side_effect = OSError("DisplayConfig failed")
            assert _real_hdr_display_active() is False


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


def _region_session(frame):
    """符合 hdrcapture.Capture.grab_region 的假会话：返回的就是区域大小的帧。"""
    session = MagicMock()
    session.grab_region.return_value = frame
    return session


class TestGrabRegionHdr:
    """长截图用的区域抓取。选屏和裁剪在 hdrcapture 里完成，这里只管调用和转 QImage。"""

    def test_passes_physical_region_and_keeps_pixel_order(self, qapp):
        from capture.capture_service import grab_region_hdr

        session = _region_session(_gradient_frame(20, 10))
        with patch("capture.capture_service._HdrSession.acquire", return_value=session):
            image = grab_region_hdr(QRect(-54, 5, 20, 10))

        assert session.grab_region.call_args.args == (-54, 5, 20, 10)
        assert (image.width(), image.height()) == (20, 10)
        color = image.pixelColor(19, 9)
        assert (color.blue(), color.green()) == (19, 9)

    def test_uses_static_tone_mapping(self, qapp):
        """长截图靠相邻帧逐像素一致拼接，映射不能随画面内容变化。"""
        from capture.capture_service import grab_region_hdr

        session = _region_session(_gradient_frame(10, 10))
        with patch("capture.capture_service._HdrSession.acquire", return_value=session):
            grab_region_hdr(QRect(0, 0, 10, 10))

        assert session.grab_region.call_args.kwargs["adaptive"] is False

    def test_raises_without_session(self, qapp):
        from capture.capture_service import grab_region_hdr

        with patch("capture.capture_service._HdrSession.acquire", return_value=None), \
             pytest.raises(RuntimeError):
            grab_region_hdr(QRect(0, 0, 10, 10))

    def test_waits_for_dwm_before_grabbing(self, qapp):
        """刚设的截图排除要等下一次 DWM 合成才进 DXGI 帧。"""
        from capture.capture_service import grab_region_hdr

        order = MagicMock()
        session = _region_session(_gradient_frame(10, 10))
        order.attach_mock(session.grab_region, "grab_region")

        with patch("capture.capture_service._HdrSession.acquire", return_value=session), \
             patch("capture.capture_service.ctypes.windll.dwmapi.DwmFlush") as flush:
            order.attach_mock(flush, "flush")
            grab_region_hdr(QRect(0, 0, 10, 10))

        calls = [name for name, *_ in order.mock_calls if name in ("flush", "grab_region")]
        assert calls == ["flush", "grab_region"]


class TestCaptureRegionHdr:
    """快速截图的选区走 HDR 会话，和主截图同样用自适应映射。"""

    def test_region_uses_adaptive_hdr_and_skips_mss(self, qapp):
        session = _region_session(_gradient_frame(20, 10))
        mock_mss = MagicMock()

        with patch("capture.capture_service._HdrSession.acquire", return_value=session), \
             patch("capture.capture_service.mss.mss", mock_mss):
            image = CaptureService("auto").capture_region(QRect(-54, 5, 20, 10))

        mock_mss.assert_not_called()
        assert session.grab_region.call_args.args == (-54, 5, 20, 10)
        assert session.grab_region.call_args.kwargs["adaptive"] is True
        assert (image.width(), image.height()) == (20, 10)

    def test_cursor_is_drawn_on_hdr_region(self, qapp):
        session = _region_session(_gradient_frame(20, 10))
        cursor = MagicMock()

        with patch("capture.capture_service._HdrSession.acquire", return_value=session):
            image = CaptureService("hdr").capture_region(QRect(8, 4, 20, 10), cursor)

        cursor.draw_onto.assert_called_once_with(image, 8, 4)

    def test_auto_falls_back_to_mss_when_hdr_grab_fails(self, qapp):
        session = MagicMock()
        session.grab_region.side_effect = RuntimeError("access lost")
        shot = MagicMock(width=4, height=3, bgra=bytes([30, 20, 10, 255] * 12))

        with patch("capture.capture_service._HdrSession.acquire", return_value=session), \
             patch("capture.capture_service.mss.mss") as mss_factory:
            mss_factory.return_value.__enter__.return_value.grab.return_value = shot
            image = CaptureService("auto").capture_region(QRect(0, 0, 4, 3))

        assert image.width() == 4

    def test_hdr_engine_raises_without_session(self, qapp):
        with patch("capture.capture_service._HdrSession.acquire", return_value=None), \
             patch("capture.capture_service.mss.mss") as mss_factory, \
             pytest.raises(RuntimeError):
            CaptureService("hdr").capture_region(QRect(0, 0, 4, 3))
        mss_factory.assert_not_called()


@pytest.fixture
def fake_hdrcapture():
    """替换 hdrcapture 模块并清空进程级会话状态；等会话线程上的任务跑完再还原。"""
    from capture.capture_service import _HdrSession

    module = MagicMock()
    with patch("capture.capture_service.hdrcapture", module), \
         patch.multiple(_HdrSession, _capture=None, _failed=False, _lent=False):
        yield module
        _HdrSession.drain()


def _thread_names(mock):
    """让 mock 每次被调用时记下所在线程名。"""
    import threading

    names = []
    mock.side_effect = lambda *args, **kwargs: names.append(threading.current_thread().name)
    return names


class TestHdrSessionLifecycle:

    def test_session_is_created_used_and_closed_on_its_own_thread(self, fake_hdrcapture):
        """pyo3 把会话钉在创建它的线程上；调用方可以在任意线程，UI 线程也不用等建会话。"""
        import threading
        from capture.capture_service import _HdrSession, apply_capture_engine

        created_on = []
        capture = MagicMock()
        fake_hdrcapture.Capture.side_effect = lambda **kwargs: (
            created_on.append(threading.current_thread().name) or capture)
        grabbed_on = _thread_names(capture.grab)
        closed_on = _thread_names(capture.close)

        _HdrSession.acquire().grab(0)
        worker = threading.Thread(target=lambda: _HdrSession.acquire().grab(0))
        worker.start()
        worker.join()
        apply_capture_engine("mss")
        _HdrSession.drain()

        assert fake_hdrcapture.Capture.call_count == 1
        threads = set(created_on + grabbed_on + closed_on)
        assert len(threads) == 1 and threads.pop().startswith("HdrCapture")

    def test_lent_session_is_closed_and_rebuilt_after_return(self, fake_hdrcapture):
        """GIF 录制线程要自建 DXGI 会话，借出期间这里既不持有也不新建。"""
        from capture.capture_service import _HdrSession, lend_hdr_session, return_hdr_session

        first = MagicMock()
        second = MagicMock()
        fake_hdrcapture.Capture.side_effect = [first, second]
        proxy = _HdrSession.acquire()
        assert proxy is not None

        assert lend_hdr_session(then=lambda: "armed").result() == "armed"
        first.close.assert_called_once()
        assert _HdrSession.acquire() is None
        with pytest.raises(RuntimeError):
            proxy.grab(0)
        assert fake_hdrcapture.Capture.call_count == 1

        order = []
        fake_hdrcapture.Capture.side_effect = lambda **kwargs: order.append("rebuilt") or second
        return_hdr_session(after=lambda: order.append("recorder stopped")).result()
        assert order == ["recorder stopped", "rebuilt"]
        assert _HdrSession._capture is second
        assert _HdrSession.acquire() is not None

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
        warm_up_hdr_session().result()
        assert _HdrSession.acquire() is not None

    def test_warm_up_keeps_session_for_capture(self, fake_hdrcapture):
        from capture.capture_service import _HdrSession, warm_up_hdr_session

        warm_up_hdr_session().result()
        assert _HdrSession.acquire() is not None
        assert fake_hdrcapture.Capture.call_count == 1

    def test_switching_to_mss_closes_session(self, fake_hdrcapture):
        from capture.capture_service import _HdrSession, apply_capture_engine

        _HdrSession.acquire()
        apply_capture_engine("mss")
        _HdrSession.drain()

        fake_hdrcapture.Capture.return_value.close.assert_called_once()
        assert _HdrSession._capture is None

    @pytest.mark.parametrize("engine", ["auto", "hdr"])
    def test_switching_back_retries_failed_session(self, fake_hdrcapture, engine):
        from capture.capture_service import _HdrSession, apply_capture_engine

        fake_hdrcapture.Capture.side_effect = [OSError("unsupported"), MagicMock()]
        assert _HdrSession.acquire() is None
        apply_capture_engine(engine)
        assert _HdrSession.acquire() is not None

    def test_auto_warm_up_skips_session_without_hdr_display(self, fake_hdrcapture, hdr_display_on):
        from capture.capture_service import _HdrSession, warm_up_hdr_session

        hdr_display_on.return_value = False
        warm_up_hdr_session("auto").result()
        fake_hdrcapture.Capture.assert_not_called()

        warm_up_hdr_session("hdr").result()
        assert _HdrSession._capture is fake_hdrcapture.Capture.return_value

    def test_turning_hdr_off_releases_session_in_background(self, fake_hdrcapture, hdr_display_on):
        from capture.capture_service import _HdrSession, apply_capture_engine

        _HdrSession.acquire()
        hdr_display_on.return_value = False
        apply_capture_engine("auto")
        _HdrSession.drain()

        fake_hdrcapture.Capture.return_value.close.assert_called_once()
        assert _HdrSession._capture is None
        assert _HdrSession._failed is False

    @staticmethod
    def _sessions_with_monitors(fake_hdrcapture, count=2):
        """每次新建都返回一个新的假会话；monitors[0] 是虚拟桌面，其后是物理屏。"""
        created = []

        def make(**kwargs):
            session = MagicMock()
            session.monitors = [MagicMock(name="virtual")] + [MagicMock(name=f"m{i}") for i in range(count)]
            created.append(session)
            return session
        fake_hdrcapture.Capture.side_effect = make
        return created

    def test_background_warm_up_waits_for_every_monitors_first_frame(self, fake_hdrcapture):
        """新会话要等一次真实 present 才有首帧，后台先等掉，首次截图的零预算抓取才拿得到。"""
        from capture.capture_service import _PRIME_TIMEOUT_MS, _HdrSession, warm_up_hdr_session

        created = self._sessions_with_monitors(fake_hdrcapture)
        warm_up_hdr_session("auto").result()

        session = created[0]
        assert [c.args[0] for c in session.grab.call_args_list] == session.monitors[1:]
        assert all(c.kwargs["timeout_ms"] == _PRIME_TIMEOUT_MS for c in session.grab.call_args_list)
        warm_up_hdr_session("auto").result()
        assert session.grab.call_count == 2, "已有会话不再预取"
        assert _HdrSession._capture is session

    def test_priming_timeout_does_not_discard_the_session(self, fake_hdrcapture):
        from capture.capture_service import _HdrSession, warm_up_hdr_session

        created = self._sessions_with_monitors(fake_hdrcapture, count=1)
        warm_up_hdr_session("auto").result()
        assert _HdrSession._capture is created[0]

        fake_hdrcapture.Capture.side_effect = None
        fake_hdrcapture.Capture.return_value.monitors = [MagicMock(), MagicMock()]
        fake_hdrcapture.Capture.return_value.grab.side_effect = RuntimeError("no present yet")
        _HdrSession.refresh().result()
        assert _HdrSession._capture is fake_hdrcapture.Capture.return_value

    def test_display_change_rebuilds_session_with_new_topology(self, fake_hdrcapture):
        from capture.capture_service import _HdrSession, refresh_hdr_session

        created = self._sessions_with_monitors(fake_hdrcapture)
        _HdrSession.acquire()
        refresh_hdr_session("auto").result()

        created[0].close.assert_called_once()
        assert _HdrSession._capture is created[1]
        assert created[1].grab.call_count == 2

    def test_display_change_that_turns_hdr_off_only_releases(self, fake_hdrcapture, hdr_display_on):
        from capture.capture_service import _HdrSession, refresh_hdr_session

        created = self._sessions_with_monitors(fake_hdrcapture)
        _HdrSession.acquire()
        hdr_display_on.return_value = False
        refresh_hdr_session("auto").result()

        created[0].close.assert_called_once()
        assert _HdrSession._capture is None
        assert len(created) == 1

    def test_display_change_retries_a_failed_session(self, fake_hdrcapture):
        from capture.capture_service import _HdrSession, refresh_hdr_session

        fake_hdrcapture.Capture.side_effect = [OSError("not ready"), MagicMock()]
        assert _HdrSession.acquire() is None
        refresh_hdr_session("hdr").result()
        assert _HdrSession.acquire() is not None

    def test_display_change_leaves_a_lent_session_alone(self, fake_hdrcapture):
        """GIF 录制线程的会话会自己发现配置变化；这里重建会和它抢 duplication。"""
        from capture.capture_service import _HdrSession, lend_hdr_session, refresh_hdr_session

        lend_hdr_session().result()
        refresh_hdr_session("auto").result()
        fake_hdrcapture.Capture.assert_not_called()
        assert _HdrSession._lent is True

    def test_display_change_is_ignored_for_mss_engine(self, fake_hdrcapture):
        from capture.capture_service import refresh_hdr_session

        assert refresh_hdr_session("mss") is None
        fake_hdrcapture.Capture.assert_not_called()

    def test_shutdown_closes_session_on_its_own_thread_before_stopping(self, fake_hdrcapture):
        """会话只能在创建它的线程上释放；关闭任务不能被当成未开始的任务取消掉。"""
        from concurrent.futures import ThreadPoolExecutor
        from capture.capture_service import _HdrSession, shutdown_hdr_session

        # 用单独的线程池，并在 fake_hdrcapture 收尾（它要往共用线程池提交任务）之前还原
        original = _HdrSession._executor, _HdrSession._worker_id
        _HdrSession._executor = ThreadPoolExecutor(max_workers=1, thread_name_prefix="HdrCapture")
        try:
            closed_on = _thread_names(fake_hdrcapture.Capture.return_value.close)
            _HdrSession.acquire()

            shutdown_hdr_session()

            assert len(closed_on) == 1 and closed_on[0].startswith("HdrCapture")
            assert _HdrSession._capture is None
        finally:
            _HdrSession._executor, _HdrSession._worker_id = original

    def test_keeping_hdr_does_not_rebuild_session(self, fake_hdrcapture):
        from capture.capture_service import _HdrSession, apply_capture_engine

        _HdrSession.acquire()
        apply_capture_engine("auto")
        _HdrSession.drain()

        fake_hdrcapture.Capture.return_value.close.assert_not_called()
        assert fake_hdrcapture.Capture.call_count == 1
