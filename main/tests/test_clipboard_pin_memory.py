"""剪贴板钉图的图片传递与内存收尾回归测试。"""

from pathlib import Path
from types import SimpleNamespace

import pytest
from PySide6.QtCore import QPoint


def test_rust_manager_returns_image_data_as_bytes(tmp_path):
    pyclipboard = pytest.importorskip("pyclipboard")
    manager = pyclipboard.ClipboardManager(str(tmp_path / "clipboard.db"))
    payload = bytes(range(256)) * 4
    image_path = Path(manager.get_images_dir()) / "probe.png"
    image_path.write_bytes(payload)

    result = manager.get_image_data("probe")

    assert type(result) is bytes
    assert result == payload


def test_manager_normalizes_legacy_image_lists_to_bytes(monkeypatch):
    from clipboard.core.manager import ClipboardManager

    manager = object.__new__(ClipboardManager)
    manager._initialized = True
    manager._manager = SimpleNamespace(get_image_data=lambda _image_id: [0, 1, 254, 255])

    assert manager.get_image_data("image-id") == b"\x00\x01\xfe\xff"


def test_clipboard_pin_schedules_the_same_working_set_trim(monkeypatch):
    import clipboard
    import core.platform_utils
    import pin
    import settings
    from clipboard.ui.windows import pin_window as module

    class FakeImage:
        def isNull(self):
            return False

        def width(self):
            return 320

        def height(self):
            return 180

    image = FakeImage()
    image_data = b"encoded png"
    created = []
    trim_delays = []

    monkeypatch.setattr(
        module,
        "QImage",
        SimpleNamespace(fromData=lambda data: image if data is image_data else None),
    )
    monkeypatch.setattr(module, "_calculate_pin_position", lambda *_args: QPoint(10, 20))
    monkeypatch.setattr(
        clipboard,
        "ClipboardManager",
        lambda: SimpleNamespace(get_image_data=lambda _image_id: image_data),
    )
    monkeypatch.setattr(settings, "get_tool_settings_manager", lambda: "config")
    monkeypatch.setattr(
        pin,
        "get_pin_manager",
        lambda: SimpleNamespace(create_pin=lambda **kwargs: created.append(kwargs)),
    )
    monkeypatch.setattr(
        core.platform_utils,
        "request_trim_working_set",
        lambda delay: trim_delays.append(delay),
    )

    controller = SimpleNamespace(
        get_item=lambda _item_id: SimpleNamespace(content_type="image", image_id="image-id")
    )

    assert module.create_pin_from_clipboard_item(7, controller) is True
    assert len(created) == 1
    assert created[0]["image"] is image
    assert created[0]["position"] == QPoint(10, 20)
    assert trim_delays == [1500]
