# -*- coding: utf-8 -*-
"""clipboard_utils 后台投递测试。"""

import threading

from unittest.mock import MagicMock

from PySide6.QtGui import QImage


def test_deliver_image_async_reuses_same_qimage(monkeypatch, tmp_path):
    from core import clipboard_utils
    from core.save import SaveService

    image = QImage(32, 24, QImage.Format.Format_ARGB32)
    image.fill(0xFF55AA33)

    mock_config = MagicMock()
    mock_config.get_screenshot_save_path.return_value = str(tmp_path)
    save_service = SaveService(config_manager=mock_config)

    seen = {}
    caller_thread = threading.get_ident()

    def fake_copy(target_image):
        seen["copy_id"] = id(target_image)
        seen["copy_thread"] = threading.get_ident()

    def fake_save(self, target_image, **kwargs):
        seen["save_id"] = id(target_image)
        seen["save_thread"] = threading.get_ident()
        seen["save_kwargs"] = kwargs
        return True, str(tmp_path / "saved.png")

    monkeypatch.setattr(clipboard_utils.sys, "platform", "win32")
    monkeypatch.setattr(clipboard_utils, "copy_image_to_clipboard", fake_copy)
    monkeypatch.setattr(SaveService, "save_qimage", fake_save)

    thread = clipboard_utils.deliver_image_async(
        image,
        save_service=save_service,
        save_kwargs={
            "directory": str(tmp_path),
            "prefix": "",
            "image_format": "PNG",
        },
    )

    assert thread is not None
    assert seen["copy_thread"] == caller_thread
    thread.join(timeout=2)
    assert not thread.is_alive()
    assert seen["copy_id"] == id(image)
    assert seen["save_id"] == id(image)
    assert seen["save_thread"] != caller_thread
    assert seen["save_kwargs"]["directory"] == str(tmp_path)


def test_copy_image_to_clipboard_uses_win32_on_win32(monkeypatch):
    from core import clipboard_utils

    image = QImage(10, 10, QImage.Format.Format_ARGB32)
    image.fill(0xFF204060)
    seen = []

    def fake_win32(target_image):
        seen.append(("win32", id(target_image)))

    def fake_qt_fallback(target_image):
        seen.append(("qt-fallback", id(target_image)))

    monkeypatch.setattr(clipboard_utils.sys, "platform", "win32")
    monkeypatch.setattr(clipboard_utils, "_copy_win32", fake_win32)
    monkeypatch.setattr(clipboard_utils, "_copy_qt_fallback", fake_qt_fallback)

    clipboard_utils.copy_image_to_clipboard(image)

    assert seen == [("win32", id(image))]


def test_copy_image_to_clipboard_falls_back_on_win32_failure(monkeypatch):
    from core import clipboard_utils

    image = QImage(10, 10, QImage.Format.Format_ARGB32)
    image.fill(0xFF406080)
    seen = []

    def fake_win32(target_image):
        seen.append(("win32", id(target_image)))
        raise RuntimeError("win32 failed")

    def fake_qt_fallback(target_image):
        seen.append(("qt-fallback", id(target_image)))

    monkeypatch.setattr(clipboard_utils.sys, "platform", "win32")
    monkeypatch.setattr(clipboard_utils, "_copy_win32", fake_win32)
    monkeypatch.setattr(clipboard_utils, "_copy_qt_fallback", fake_qt_fallback)

    clipboard_utils.copy_image_to_clipboard(image)

    assert seen == [("win32", id(image)), ("qt-fallback", id(image))]


def test_copy_image_to_clipboard_uses_qt_fallback_off_windows(monkeypatch):
    from core import clipboard_utils

    image = QImage(10, 10, QImage.Format.Format_ARGB32)
    image.fill(0xFF406080)
    seen = []

    def fake_win32(target_image):
        seen.append(("win32", id(target_image)))

    def fake_qt_fallback(target_image):
        seen.append(("qt-fallback", id(target_image)))

    monkeypatch.setattr(clipboard_utils.sys, "platform", "linux")
    monkeypatch.setattr(clipboard_utils, "_copy_win32", fake_win32)
    monkeypatch.setattr(clipboard_utils, "_copy_qt_fallback", fake_qt_fallback)

    clipboard_utils.copy_image_to_clipboard(image)

    assert seen == [("qt-fallback", id(image))]


def test_pin_window_copy_to_clipboard_dispatches_async(monkeypatch):
    from pin.pin_window import PinWindow
    import pin.pin_window as pin_window_module

    image = QImage(20, 12, QImage.Format.Format_ARGB32)
    image.fill(0xFF224466)
    seen = {}

    def fake_deliver(target_image):
        seen["image_id"] = id(target_image)

    class FakePinWindow:
        def __init__(self):
            self.image = image

        def get_current_image(self):
            return self.image

        def _with_edit_paused(self, func):
            seen["paused"] = True
            func()

    monkeypatch.setattr(pin_window_module, "deliver_image_async", fake_deliver)

    fake_window = FakePinWindow()
    PinWindow.copy_to_clipboard(fake_window)

    assert seen["paused"] is True
    assert seen["image_id"] == id(image)


def test_canvas_view_export_and_close_dispatches_async(monkeypatch):
    from canvas.view import CanvasView
    from core import clipboard_utils
    import core.export as export_module

    image = QImage(18, 10, QImage.Format.Format_ARGB32)
    image.fill(0xFF6688AA)
    seen = {}

    class FakeExporter:
        def __init__(self, scene):
            seen["scene"] = scene

        def export(self, selection_rect):
            seen["selection_rect"] = selection_rect
            return image

    class FakeSelectionModel:
        def rect(self):
            return "selection-rect"

    class FakeScene:
        selection_model = FakeSelectionModel()

    class FakeWindow:
        def close(self):
            seen["closed"] = True

    class FakeCanvasView:
        canvas_scene = FakeScene()

        def window(self):
            return FakeWindow()

    def fake_deliver(target_image):
        seen["image_id"] = id(target_image)

    monkeypatch.setattr(export_module, "ExportService", FakeExporter)
    monkeypatch.setattr(clipboard_utils, "deliver_image_async", fake_deliver)

    fake_view = FakeCanvasView()
    CanvasView.export_and_close(fake_view)

    assert seen["scene"] is fake_view.canvas_scene
    assert seen["selection_rect"] == "selection-rect"
    assert seen["image_id"] == id(image)
    assert seen["closed"] is True