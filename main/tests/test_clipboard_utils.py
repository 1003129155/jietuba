# -*- coding: utf-8 -*-
"""clipboard_utils 后台投递测试。"""

import sys
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

    def fake_copy(target_image, file_reference=None):
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


def test_deliver_image_async_shares_reserved_path_with_clipboard_and_save(monkeypatch, tmp_path):
    """复制与保存应共用同一个提前占用好的路径，而不是各自决定文件名。"""
    from core import clipboard_utils
    from core.save import SaveService

    image = QImage(16, 16, QImage.Format.Format_ARGB32)
    image.fill(0xFF112233)

    mock_config = MagicMock()
    mock_config.get_screenshot_save_path.return_value = str(tmp_path)
    save_service = SaveService(config_manager=mock_config)
    reserved_path = str(tmp_path / "reserved.png")

    seen = {}

    monkeypatch.setattr(clipboard_utils.sys, "platform", "win32")
    monkeypatch.setattr(SaveService, "reserve_save_path", lambda self, **kw: reserved_path)
    monkeypatch.setattr(
        clipboard_utils,
        "copy_image_to_clipboard",
        lambda img, file_reference=None: seen.update(file_reference=file_reference),
    )

    def fake_save(self, target_image, **kwargs):
        seen["save_target_path"] = kwargs.get("target_path")
        return True, reserved_path

    monkeypatch.setattr(SaveService, "save_qimage", fake_save)

    thread = clipboard_utils.deliver_image_async(
        image,
        save_service=save_service,
        save_kwargs={"directory": str(tmp_path), "prefix": "", "image_format": "PNG"},
    )
    thread.join(timeout=2)

    assert seen["file_reference"] == reserved_path
    assert seen["save_target_path"] == reserved_path


def test_deliver_image_async_still_copies_when_reservation_fails(monkeypatch, tmp_path):
    """保存目录不可写时仍要复制成功：占不到路径只是少一个文件引用格式。"""
    from core import clipboard_utils
    from core.save import SaveService

    image = QImage(8, 8, QImage.Format.Format_ARGB32)
    image.fill(0xFF445566)

    mock_config = MagicMock()
    mock_config.get_screenshot_save_path.return_value = str(tmp_path)
    save_service = SaveService(config_manager=mock_config)

    seen = {}

    def raise_on_reserve(self, **kwargs):
        raise OSError("目标目录不可写")

    monkeypatch.setattr(clipboard_utils.sys, "platform", "win32")
    monkeypatch.setattr(SaveService, "reserve_save_path", raise_on_reserve)
    monkeypatch.setattr(
        clipboard_utils,
        "copy_image_to_clipboard",
        lambda img, file_reference=None: seen.update(file_reference=file_reference, copied=True),
    )
    monkeypatch.setattr(SaveService, "save_qimage", lambda self, img, **kw: (False, None))

    thread = clipboard_utils.deliver_image_async(
        image,
        save_service=save_service,
        save_kwargs={"directory": str(tmp_path), "prefix": "", "image_format": "PNG"},
    )
    thread.join(timeout=2)

    assert seen["copied"] is True
    assert seen["file_reference"] is None


def test_deliver_image_async_skips_reservation_without_save_service(monkeypatch):
    """不需要保存时，不应提前占用路径，也不该给剪贴板挂无意义的文件引用。"""
    from core import clipboard_utils

    image = QImage(8, 8, QImage.Format.Format_ARGB32)
    image.fill(0xFF001122)
    seen = {}

    monkeypatch.setattr(clipboard_utils.sys, "platform", "win32")
    monkeypatch.setattr(
        clipboard_utils,
        "copy_image_to_clipboard",
        lambda img, file_reference=None: seen.update(file_reference=file_reference),
    )

    result = clipboard_utils.deliver_image_async(image, save_service=None)

    assert result is None
    assert seen["file_reference"] is None


def test_copy_win32_writes_hdrop_in_single_clipboard_transaction(monkeypatch):
    """CF_HDROP 必须和位图格式在同一次 Open/Close 里写入。

    分两次 OpenClipboard/CloseClipboard 会被剪贴板历史一类的监听者
    当成两次独立的"内容变化"，为同一张截图生成两条历史记录。
    """
    from core import clipboard_utils

    image = QImage(4, 4, QImage.Format.Format_ARGB32)
    image.fill(0xFF334455)

    calls = []

    class FakeWin32Clipboard:
        @staticmethod
        def RegisterClipboardFormat(name):
            calls.append(("RegisterClipboardFormat", name))
            return 49999

        @staticmethod
        def OpenClipboard(_):
            calls.append(("OpenClipboard",))

        @staticmethod
        def EmptyClipboard():
            calls.append(("EmptyClipboard",))

        @staticmethod
        def SetClipboardData(fmt, data):
            calls.append(("SetClipboardData", fmt))

        @staticmethod
        def CloseClipboard():
            calls.append(("CloseClipboard",))

    monkeypatch.setattr(clipboard_utils.sys, "platform", "win32")
    monkeypatch.setitem(sys.modules, "win32clipboard", FakeWin32Clipboard)

    clipboard_utils.copy_image_to_clipboard(image, file_reference=r"C:\shot.png")

    assert calls == [
        ("RegisterClipboardFormat", "PNG"),
        ("OpenClipboard",),
        ("EmptyClipboard",),
        ("SetClipboardData", 49999),
        ("SetClipboardData", 17),
        ("SetClipboardData", 15),
        ("CloseClipboard",),
    ]


def test_copy_win32_omits_hdrop_without_file_reference(monkeypatch):
    from core import clipboard_utils

    image = QImage(4, 4, QImage.Format.Format_ARGB32)
    image.fill(0xFF334455)

    formats_set = []

    class FakeWin32Clipboard:
        @staticmethod
        def RegisterClipboardFormat(name):
            return 49999

        @staticmethod
        def OpenClipboard(_):
            pass

        @staticmethod
        def EmptyClipboard():
            pass

        @staticmethod
        def SetClipboardData(fmt, data):
            formats_set.append(fmt)

        @staticmethod
        def CloseClipboard():
            pass

    monkeypatch.setattr(clipboard_utils.sys, "platform", "win32")
    monkeypatch.setitem(sys.modules, "win32clipboard", FakeWin32Clipboard)

    clipboard_utils.copy_image_to_clipboard(image)

    assert formats_set == [49999, 17]


def test_build_hdrop_round_trips_via_win32clipboard():
    import win32clipboard
    from core import clipboard_utils

    path = r"C:\Users\10031\Desktop\测试 图片.png"
    payload = clipboard_utils._build_hdrop(path)

    win32clipboard.OpenClipboard(0)
    try:
        win32clipboard.EmptyClipboard()
        win32clipboard.SetClipboardData(15, payload)  # CF_HDROP
        result = win32clipboard.GetClipboardData(15)
    finally:
        win32clipboard.CloseClipboard()

    assert result == (path,)


def test_copy_image_to_clipboard_uses_win32_on_win32(monkeypatch):
    from core import clipboard_utils

    image = QImage(10, 10, QImage.Format.Format_ARGB32)
    image.fill(0xFF204060)
    seen = []

    def fake_win32(target_image, file_reference=None):
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

    def fake_win32(target_image, file_reference=None):
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

    def fake_win32(target_image, file_reference=None):
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