# -*- coding: utf-8 -*-

"""管理窗口保存逻辑服务。

封装 ManageDialog 中与持久化相关的动作，包括新建或更新分组、
保存文本条目和文件条目，并兼容不同 manager 方法签名。
"""

from dataclasses import dataclass
import inspect
from typing import Optional

from .file_payload_service import build_file_payload


@dataclass
class SaveResult:
    success: bool
    action: str
    error: Optional[str] = None
    entity_id: Optional[int] = None


def save_group(manager, editing_group_id: Optional[int], name: str, icon: Optional[str], group_type: int) -> SaveResult:
    """保存分组，兼容不同 manager 方法签名。"""
    if editing_group_id is None:
        group_id = _call_with_supported_kwargs(
            manager.create_group,
            name,
            icon=icon,
            group_type=group_type,
        )
        if group_id:
            return SaveResult(success=True, action="created", entity_id=group_id)
        return SaveResult(success=False, action="created", error="create_group_failed")

    updated = _call_with_supported_kwargs(
        manager.update_group,
        editing_group_id,
        name,
        icon=icon,
        group_type=group_type,
    )
    if updated:
        return SaveResult(success=True, action="updated", entity_id=editing_group_id)
    return SaveResult(success=False, action="updated", error="update_group_failed")


def save_text_content(manager, editing_item_id: Optional[int], selected_group_id: int, content: str, title: Optional[str]) -> SaveResult:
    """保存文本内容。"""
    if editing_item_id is None:
        item_id = manager.add_item(content, "text", title=title)
        if not item_id:
            return SaveResult(success=False, action="created", error="add_content_failed")
        if not manager.move_to_group(item_id, selected_group_id):
            return SaveResult(success=False, action="created", error="move_to_group_failed", entity_id=item_id)
        return SaveResult(success=True, action="created", entity_id=item_id)

    if manager.update_item(editing_item_id, content, title=title):
        return SaveResult(success=True, action="updated", entity_id=editing_item_id)
    return SaveResult(success=False, action="updated", error="update_content_failed", entity_id=editing_item_id)


def save_file_content(manager, editing_item_id: Optional[int], selected_group_id: int, path: str, title: Optional[str]) -> SaveResult:
    """保存文件内容。"""
    payload = build_file_payload(path)
    if editing_item_id is None:
        item_id = manager.add_item(payload, "file", title=title)
        if not item_id:
            return SaveResult(success=False, action="created", error="add_content_failed")
        if not manager.move_to_group(item_id, selected_group_id):
            return SaveResult(success=False, action="created", error="move_to_group_failed", entity_id=item_id)
        return SaveResult(success=True, action="created", entity_id=item_id)

    if manager.update_item(editing_item_id, payload, title=title):
        return SaveResult(success=True, action="updated", entity_id=editing_item_id)
    return SaveResult(success=False, action="updated", error="update_content_failed", entity_id=editing_item_id)


def _call_with_supported_kwargs(method, *args, **kwargs):
    """仅向方法传递其签名中支持的关键字参数。"""
    try:
        parameters = inspect.signature(method).parameters
    except (TypeError, ValueError):
        return method(*args, **kwargs)

    supported_kwargs = {name: value for name, value in kwargs.items() if name in parameters}
    return method(*args, **supported_kwargs)
