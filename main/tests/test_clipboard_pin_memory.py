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


def test_latest_history_image_is_picked_by_created_at(monkeypatch):
    """历史里的「最新」按入库时间取，而不是列表顺序。

    get_history 把置顶项排在最前，直接拿第一条会钉到一张被置顶的旧图。
    """
    from datetime import datetime

    import clipboard
    import settings
    from clipboard.ui.windows import pin_window as module

    rows = [
        # 置顶的旧图排在最前，正是列表顺序会踩的坑
        SimpleNamespace(id=1, image_id="old-pinned", created_at=datetime(2026, 1, 1)),
        SimpleNamespace(id=2, image_id="newest", created_at=datetime(2026, 3, 1)),
        SimpleNamespace(id=3, image_id="middle", created_at=datetime(2026, 2, 1)),
    ]
    fake_manager = SimpleNamespace(
        is_available=True,
        get_history=lambda _offset, _limit, content_type=None: (
            rows if content_type == "image" else []
        ),
    )
    monkeypatch.setattr(settings, "get_tool_settings_manager",
                        lambda: SimpleNamespace(get_clipboard_enabled=lambda: True))
    monkeypatch.setattr(clipboard, "ClipboardManager", lambda: fake_manager)

    manager, item = module._find_latest_history_image()

    assert manager is fake_manager
    assert item.id == 2


def test_latest_history_image_skipped_when_clipboard_disabled(monkeypatch):
    """剪贴板监听关掉时不去碰历史库，免得把数据库平白拉起来。"""
    import clipboard
    import settings
    from clipboard.ui.windows import pin_window as module

    def _boom():
        raise AssertionError("不应该创建 ClipboardManager")

    monkeypatch.setattr(settings, "get_tool_settings_manager",
                        lambda: SimpleNamespace(get_clipboard_enabled=lambda: False))
    monkeypatch.setattr(clipboard, "ClipboardManager", _boom)

    assert module._find_latest_history_image() is None


def test_pin_latest_prefers_the_system_clipboard_image(monkeypatch):
    """系统剪贴板本身就是图时直接钉它，不查历史。"""
    from clipboard.ui.windows import pin_window as module

    image = object()
    pinned = []

    monkeypatch.setattr(module, "_read_system_clipboard_image", lambda: image)
    monkeypatch.setattr(
        module,
        "_find_latest_history_image",
        lambda: (_ for _ in ()).throw(AssertionError("不应该查历史")),
    )
    monkeypatch.setattr(
        module,
        "create_pin_from_image",
        lambda img, clipboard_window=None, center=None: pinned.append((img, center)) or True,
    )

    assert module.pin_latest_clipboard_image(QPoint(800, 600)) is True
    assert pinned == [(image, QPoint(800, 600))]


def test_pin_position_centers_on_the_given_point(qapp):
    """热键钉图以鼠标为中心，不再跑到屏幕正中。"""
    from PySide6.QtGui import QGuiApplication

    from clipboard.ui.windows import pin_window as module

    screen = QGuiApplication.primaryScreen()
    geometry = screen.geometry()
    center = QPoint(
        geometry.x() + geometry.width() // 2,
        geometry.y() + geometry.height() // 2,
    )
    image = SimpleNamespace(width=lambda: 200, height=lambda: 100)

    position = module._calculate_pin_position(image, None, center)

    assert position == QPoint(center.x() - 100, center.y() - 50)


def test_system_clipboard_with_text_is_not_treated_as_image(monkeypatch):
    """带文本的剪贴板不算图片——和 Rust 监听端写历史的优先级保持一致。"""
    from clipboard.ui.windows import pin_window as module

    class FakeMime:
        def hasUrls(self):
            return False

        def hasText(self):
            return True

        def text(self):
            return "A1\tB1"

    class FakeImage:
        def isNull(self):
            return False

    fake_clipboard = SimpleNamespace(
        mimeData=lambda: FakeMime(),
        image=lambda: FakeImage(),
    )
    monkeypatch.setattr(
        module, "QGuiApplication",
        SimpleNamespace(clipboard=lambda: fake_clipboard),
    )

    assert module._read_system_clipboard_image() is None
