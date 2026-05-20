# -*- coding: utf-8 -*-
"""clipboard 模块导出入口。"""

from .core import ClipboardItem, ClipboardManager, Group, GroupType
from .ui.dialogs.manage_dialog import ManageDialog, get_existing_manage_dialog, get_manage_dialog
from .ui.windows.clipboard_window import ClipboardWindow

__all__ = [
    "ClipboardItem",
    "ClipboardManager",
    "ClipboardWindow",
    "Group",
    "GroupType",
    "ManageDialog",
    "get_existing_manage_dialog",
    "get_manage_dialog",
]

 