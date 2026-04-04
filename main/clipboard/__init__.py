# -*- coding: utf-8 -*-
"""
剪贴板管理模块

提供剪贴板历史管理功能，类似 Ditto。

主要组件：
- ClipboardManager: 剪贴板管理器（封装 pyclipboard）
- ClipboardWindow: 剪贴板历史窗口
- ClipboardItem: 剪贴板项数据类
- ManageDialog: 剪贴板管理窗口（独立窗口）
- get_manage_dialog: 获取管理窗口单例

依赖：
- pyclipboard (Rust 实现的剪贴板库)
"""

from .data_manager import ClipboardManager
from .window import ClipboardWindow
from .data_setting import ManageDialog, get_manage_dialog

__all__ = [
    'ClipboardManager',
    'ClipboardWindow',
    'ManageDialog',
    'get_manage_dialog',
]
 