"""clipboard core 数据模型。

定义剪贴板模块共享的数据对象，主要包括：
- `ClipboardItem`：单条剪贴板记录及其展示相关属性
- `Group`：分组名称、图标、类型等基础信息

这些模型尽量保持 UI 无关，供 core、controller 和 UI 层共同使用。
"""

import json
from dataclasses import dataclass
from datetime import datetime
from typing import Optional, TYPE_CHECKING

from core.logger import T, log_exception

from .enums import GroupType

if TYPE_CHECKING:
    # 仅供类型注解使用：pyclipboard 是自制 Rust 扩展，运行环境未安装时
    # core.manager 会降级处理，所以这里不能在运行时导入。
    from pyclipboard import PyClipboardItem, PyGroup


@dataclass
class ClipboardItem:
    """剪贴板项数据类。"""

    id: int
    content: str
    content_type: str
    title: Optional[str] = None
    html_content: Optional[str] = None
    image_id: Optional[str] = None
    thumbnail: Optional[str] = None
    is_pinned: bool = False
    paste_count: int = 0
    source_app: Optional[str] = None
    created_at: datetime = None
    updated_at: datetime = None

    @classmethod
    def from_py_item(cls, item: "PyClipboardItem") -> "ClipboardItem":
        return cls(
            id=item.id,
            content=item.content,
            content_type=item.content_type,
            title=item.title,
            html_content=item.html_content,
            image_id=item.image_id,
            thumbnail=item.thumbnail,
            is_pinned=item.is_pinned,
            paste_count=item.paste_count,
            source_app=item.source_app,
            created_at=datetime.fromtimestamp(item.created_at) if item.created_at else None,
            updated_at=datetime.fromtimestamp(item.updated_at) if item.updated_at else None,
        )

    @property
    def display_text(self) -> str:
        if self.title:
            return self.title

        if self.content_type == "text":
            text = self.content.replace("\n", " ").strip()
            return text[:100] + "..." if len(text) > 100 else text
        if self.content_type == "image":
            return self.content.strip("[]")
        if self.content_type == "file":
            try:
                import os

                data = json.loads(self.content)
                files = data.get("files", [])
                if len(files) == 1:
                    return os.path.basename(files[0])
                if len(files) > 1:
                    return ", ".join(os.path.basename(file_path) for file_path in files)
                return "文件"
            except Exception as e:
                log_exception(e, T("解析文件类型显示文本"))
                return "文件"
        return self.content[:50]

    @property
    def icon(self) -> str:
        if self.content_type == "file":
            return "📁"
        if self.content_type == "image":
            return "📷"
        return ""


@dataclass
class Group:
    """分组数据类。"""

    id: int
    name: str
    color: Optional[str] = None
    icon: Optional[str] = None
    group_type: int = GroupType.NORMAL

    @classmethod
    def from_py_group(cls, group: "PyGroup") -> "Group":
        return cls(
            id=group.id,
            name=group.name,
            color=group.color,
            icon=group.icon,
            group_type=group.group_type,
        )


__all__ = ["ClipboardItem", "Group"]