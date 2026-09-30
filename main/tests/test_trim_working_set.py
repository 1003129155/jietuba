"""共用的内存回收：连续请求只执行一次、忙时顺延、先回收再裁剪，以及哪些地方会请求它。

不碰真实的工作集：裁剪和 gc 都换成记录调用。
"""

from types import SimpleNamespace
from unittest.mock import Mock

import pytest
from PySide6.QtCore import QRectF
from PySide6.QtGui import QImage

from core import platform_utils
from main_app import MainApp


@pytest.fixture
def trims(qapp, monkeypatch):
    calls = []
    monkeypatch.setattr(platform_utils, "trim_working_set", lambda: calls.append("trim"))
    monkeypatch.setattr(platform_utils, "gc", SimpleNamespace(collect=lambda: calls.append("gc")))
    monkeypatch.setattr(platform_utils, "_trim_timer", None)
    monkeypatch.setattr(platform_utils, "_trim_busy", None)
    yield calls
    if platform_utils._trim_timer is not None:
        platform_utils._trim_timer.stop()


@pytest.fixture
def requests(monkeypatch):
    delays = []
    monkeypatch.setattr(platform_utils, "request_trim_working_set", lambda delay=1500: delays.append(delay))
    return delays


def test_repeated_requests_collect_and_trim_once_after_the_last(trims, qtbot):
    for _ in range(5):
        platform_utils.request_trim_working_set(60)
        qtbot.wait(20)
    assert trims == []
    qtbot.waitUntil(lambda: trims == ["gc", "trim"])
    qtbot.wait(150)
    assert trims == ["gc", "trim"]


def test_busy_postpones_until_idle_instead_of_skipping(trims, qtbot):
    busy = [True]
    platform_utils.set_trim_busy_check(lambda: busy[0])
    platform_utils.request_trim_working_set(30)
    qtbot.wait(150)
    assert trims == []
    busy[0] = False
    qtbot.waitUntil(lambda: trims == ["gc", "trim"])


@pytest.mark.parametrize("state,busy", [
    ({}, False),
    ({"_capture_pending": True}, True),
    ({"screenshot_window": SimpleNamespace(_session_active=True)}, True),
    ({"screenshot_window": SimpleNamespace(_session_active=False)}, False),
    ({"quick_capture": SimpleNamespace(busy=True)}, True),
])
def test_capture_work_counts_as_busy(state, busy):
    app = SimpleNamespace(**{"_capture_pending": False, "screenshot_window": None,
                             "quick_capture": SimpleNamespace(busy=False), **state})
    assert MainApp._capture_busy(app) is busy


def test_closing_pins_requests_a_trim(qapp, requests):
    from pin.pin_manager import PinManager

    manager = PinManager.instance()
    first, second = object(), object()
    manager.pin_windows.extend([first, second])
    try:
        manager._on_pin_closed(first)
        manager._on_pin_closed(second)
        manager._on_pin_closed(second)  # 重复的关闭通知不再请求
    finally:
        manager.pin_windows.clear()
    assert requests == [1500, 1500]


def test_pin_recognition_requests_a_trim_when_it_finishes(qapp, requests):
    from pin.pin_ocr_manager import PinOCRManager

    manager = PinOCRManager(SimpleNamespace(_is_closed=True), None)
    thread = Mock()
    manager.ocr_thread = thread
    manager._on_finished(100, 50)
    thread.deleteLater.assert_called_once_with()
    assert requests == [1500]


def test_text_recognition_requests_a_trim_when_the_thread_finishes(qapp, monkeypatch, requests):
    from text_recognition import recognize_async

    monkeypatch.setattr("ocr.is_ocr_available", lambda: True)
    monkeypatch.setattr("ocr.recognize_text", lambda image, **kwargs: {"code": 101, "data": []})
    thread = recognize_async(QImage(20, 10, QImage.Format.Format_RGB32), lambda *_args: None)
    assert thread.wait(5000)
    qapp.processEvents()
    assert requests == [1500]


def test_translation_recognition_requests_a_trim_even_if_the_dialog_is_gone(requests):
    from translation.translation_manager import TranslationManager

    manager = SimpleNamespace(_pending_pixmap=object(), _is_dialog_valid=lambda: False)
    TranslationManager._on_ocr_finished(manager, True, "text")
    assert manager._pending_pixmap is None
    assert requests == [1500]


def test_screenshot_session_end_still_requests_a_trim(qapp, tmp_settings, monkeypatch, requests):
    from pin.pin_manager import PinManager
    from settings.tool_settings import ToolSettingsManager
    from ui.screenshot_window import ScreenshotWindow

    monkeypatch.setattr(PinManager, "suppress_topmost", lambda self: None)
    monkeypatch.setattr(PinManager, "restore_topmost", lambda self: None)
    config = ToolSettingsManager(tmp_settings)
    image = QImage(200, 100, QImage.Format.Format_ARGB32)
    window = ScreenshotWindow(config, prefetched_image=image, prefetched_rect=QRectF(0, 0, 200, 100))
    try:
        window.cleanup_and_close()
    finally:
        window.deleteLater()
    assert requests == [1500]
