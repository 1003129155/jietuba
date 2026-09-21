# -*- coding: utf-8 -*-
"""ActionTools._copy_save_and_close —— 剪贴板文件引用开关的读取与透传。

"写入文件路径到剪贴板"依赖自动保存先落盘拿到路径，所以只在自动保存
开启时才读取这个子开关；自动保存关闭时不传文件引用，也不应该去读
这个可能不存在于旧配置里的子开关。
"""
from types import SimpleNamespace
from unittest.mock import MagicMock

from PySide6.QtCore import QRectF

from tools.action import ActionTools


def _make_action_tools(config_manager):
    tools = ActionTools.__new__(ActionTools)
    tools.scene = None
    tools.parent_window = None
    tools.save_service = MagicMock(name="save_service")
    tools.config_manager = config_manager
    fake_image = object()
    tools.export_service = SimpleNamespace(
        scene=SimpleNamespace(
            selection_model=SimpleNamespace(rect=lambda: QRectF(0, 0, 10, 10))
        ),
        export=lambda rect: fake_image,
    )
    return tools, fake_image


def test_copy_save_and_close_reads_file_reference_toggle_when_auto_save_enabled(monkeypatch):
    config_manager = MagicMock()
    config_manager.get_screenshot_save_enabled.return_value = True
    config_manager.get_screenshot_format.return_value = "PNG"
    config_manager.get_screenshot_save_path.return_value = "C:/shots"
    config_manager.get_clipboard_file_reference_enabled.return_value = False

    tools, fake_image = _make_action_tools(config_manager)

    seen = {}
    monkeypatch.setattr(
        "core.clipboard_utils.deliver_image_async",
        lambda image, **kwargs: seen.update(image=image, **kwargs),
    )

    tools._copy_save_and_close()

    assert seen["image"] is fake_image
    assert seen["save_service"] is tools.save_service
    assert seen["write_file_reference"] is False
    config_manager.get_clipboard_file_reference_enabled.assert_called_once()


def test_copy_save_and_close_skips_file_reference_lookup_when_auto_save_disabled(monkeypatch):
    config_manager = MagicMock()
    config_manager.get_screenshot_save_enabled.return_value = False

    tools, fake_image = _make_action_tools(config_manager)

    seen = {}
    monkeypatch.setattr(
        "core.clipboard_utils.deliver_image_async",
        lambda image, **kwargs: seen.update(image=image, **kwargs),
    )

    tools._copy_save_and_close()

    assert seen["save_service"] is None
    assert seen["write_file_reference"] is True
    config_manager.get_clipboard_file_reference_enabled.assert_not_called()
