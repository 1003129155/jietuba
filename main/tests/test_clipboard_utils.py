# -*- coding: utf-8 -*-
"""clipboard_utils 后台投递测试。"""

import win32con

from unittest.mock import MagicMock

import pythoncom
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

    def fake_copy(target_image):
        seen["copy_id"] = id(target_image)

    def fake_save(self, target_image, **kwargs):
        seen["save_id"] = id(target_image)
        seen["save_kwargs"] = kwargs
        return True, str(tmp_path / "saved.png")

    monkeypatch.setattr(clipboard_utils.sys, "platform", "win32")
    monkeypatch.setattr(clipboard_utils, "_copy_win32", fake_copy)
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
    thread.join(timeout=2)
    assert not thread.is_alive()
    assert seen["copy_id"] == id(image)
    assert seen["save_id"] == id(image)
    assert seen["save_kwargs"]["directory"] == str(tmp_path)


def test_copy_win32_prefers_ole_path(monkeypatch):
    from core import clipboard_utils

    image = QImage(10, 10, QImage.Format.Format_ARGB32)
    image.fill(0xFF204060)
    seen = []

    def fake_ole(target_image):
        seen.append(("ole", id(target_image)))

    def fake_legacy(target_image):
        seen.append(("legacy", id(target_image)))

    monkeypatch.setattr(clipboard_utils, "_copy_win32_ole", fake_ole)
    monkeypatch.setattr(clipboard_utils, "_copy_win32_legacy", fake_legacy)

    clipboard_utils._copy_win32(image)

    assert seen == [("ole", id(image))]


def test_copy_win32_falls_back_to_legacy_when_ole_fails(monkeypatch):
    from core import clipboard_utils

    image = QImage(10, 10, QImage.Format.Format_ARGB32)
    image.fill(0xFF406080)
    seen = []

    def fake_ole(target_image):
        seen.append(("ole", id(target_image)))
        raise RuntimeError("ole failed")

    def fake_legacy(target_image):
        seen.append(("legacy", id(target_image)))

    monkeypatch.setattr(clipboard_utils, "_copy_win32_ole", fake_ole)
    monkeypatch.setattr(clipboard_utils, "_copy_win32_legacy", fake_legacy)

    clipboard_utils._copy_win32(image)

    assert seen == [("ole", id(image)), ("legacy", id(image))]


def test_copy_win32_retries_ole_when_clipboard_busy(monkeypatch):
    from core import clipboard_utils

    image = QImage(10, 10, QImage.Format.Format_ARGB32)
    image.fill(0xFF406080)
    seen = {"ole": 0, "legacy": 0}

    class BusyClipboardError(Exception):
        hresult = clipboard_utils._CLIPBOARD_BUSY_HRESULT

    def fake_ole(target_image):
        seen["ole"] += 1
        if seen["ole"] < 3:
            raise BusyClipboardError("clipboard busy")

    def fake_legacy(target_image):
        seen["legacy"] += 1

    monkeypatch.setattr(clipboard_utils, "_copy_win32_ole", fake_ole)
    monkeypatch.setattr(clipboard_utils, "_copy_win32_legacy", fake_legacy)
    monkeypatch.setattr(clipboard_utils, "_CLIPBOARD_WRITE_RETRY_DELAYS", (0.0, 0.0, 0.0))

    clipboard_utils._copy_win32(image)

    assert seen == {"ole": 3, "legacy": 0}


def test_ole_data_object_prefers_png_before_bitmap():
    from core import clipboard_utils

    image = QImage(12, 9, QImage.Format.Format_ARGB32)
    image.fill(0x80224466)

    data_object = clipboard_utils._OleClipboardImageDataObject(image)
    supported = data_object.supported_formats

    assert supported[0][0] == win32con.CF_BITMAP
    assert supported[0][4] == pythoncom.TYMED_GDI
    assert supported[1][0] == data_object.png_format
    assert supported[1][4] == pythoncom.TYMED_ISTREAM
    assert supported[2][0] == data_object.cloud_format
    assert data_object.history_format in [fmt[0] for fmt in supported]
    assert data_object.history_format in [fmt[0] for fmt in supported]


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