# -*- coding: utf-8 -*-
"""剪贴板内容右键菜单 UI 构建。"""

from PySide6.QtCore import QPoint
from PySide6.QtWidgets import QWidget

from ...controllers.context_menu_controller import ContextMenuData
from .action_menu import ActionHandler, ClipboardActionMenu, DynamicHandlerResolver


class ClipboardItemContextMenu(ClipboardActionMenu):
    """根据 controller 产出的内容菜单数据渲染 QMenu。"""

    def show(self, anchor_widget: QWidget, pos: QPoint, context_data: ContextMenuData):
        self.show_actions(anchor_widget, pos, context_data.actions)


__all__ = [
    "ActionHandler",
    "ClipboardItemContextMenu",
    "DynamicHandlerResolver",
]