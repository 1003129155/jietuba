# -*- coding: utf-8 -*-
"""长截图取帧：先走 HDR，失败后本次长截图剩下的帧都走 grabWindow。

不实例化 ScrollCaptureWindow（__init__ 会建 QTimer、挂鼠标钩子），以未绑定方式调用
_grab_capture_rect，用 SimpleNamespace 充当 self。
"""
from types import SimpleNamespace
from unittest.mock import MagicMock, patch

from PySide6.QtCore import QRect
from PySide6.QtGui import QImage

from stitch.scroll_window import ScrollCaptureWindow


def _image(width, height):
    image = QImage(width, height, QImage.Format.Format_RGB32)
    image.fill(0)
    return image


def _fake_self():
    return SimpleNamespace(capture_rect=QRect(100, 200, 300, 400), _use_hdr=True)


def _fake_gdi_app(image):
    screen = MagicMock()
    screen.geometry.return_value = QRect(0, 0, 2560, 1440)
    screen.grabWindow.return_value.isNull.return_value = False
    screen.grabWindow.return_value.toImage.return_value = image
    app = MagicMock()
    app.screenAt.return_value = screen
    return app, screen


def test_uses_hdr_when_available(qapp):
    fake = _fake_self()
    hdr_image = _image(300, 400)
    app, screen = _fake_gdi_app(_image(300, 400))

    with patch("stitch.scroll_window.grab_region_hdr", return_value=hdr_image) as grab, \
         patch("stitch.scroll_window.QGuiApplication.instance", return_value=app):
        result = ScrollCaptureWindow._grab_capture_rect(fake)

    assert result is hdr_image
    grab.assert_called_once_with(fake.capture_rect)
    screen.grabWindow.assert_not_called()


def test_hdr_failure_switches_rest_of_session_to_gdi(qapp):
    """拼接靠相邻帧逐像素一致，失败一次后不能再回到 HDR。"""
    fake = _fake_self()
    gdi_image = _image(300, 400)
    app, screen = _fake_gdi_app(gdi_image)

    with patch("stitch.scroll_window.grab_region_hdr",
               side_effect=RuntimeError("HDR capture session unavailable")) as grab, \
         patch("stitch.scroll_window.QGuiApplication.instance", return_value=app):
        first = ScrollCaptureWindow._grab_capture_rect(fake)
        second = ScrollCaptureWindow._grab_capture_rect(fake)

    assert first is gdi_image and second is gdi_image
    assert grab.call_count == 1
    assert fake._use_hdr is False
    screen.grabWindow.assert_called_with(0, 100, 200, 300, 400)


def test_gdi_null_pixmap_reports_failure(qapp):
    fake = _fake_self()
    fake._use_hdr = False
    app, screen = _fake_gdi_app(_image(300, 400))
    screen.grabWindow.return_value.isNull.return_value = True

    with patch("stitch.scroll_window.QGuiApplication.instance", return_value=app):
        assert ScrollCaptureWindow._grab_capture_rect(fake) is None
