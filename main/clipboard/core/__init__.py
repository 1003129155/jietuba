"""clipboard core 入口。"""

from .enums import GroupType
from .manager import ClipboardManager
from .models import ClipboardItem, Group

__all__ = ["ClipboardItem", "ClipboardManager", "Group", "GroupType"]