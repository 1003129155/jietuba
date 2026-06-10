# -*- coding: utf-8 -*-
"""剪贴板分组右键菜单 UI 构建。"""

from typing import Sequence

from PySide6.QtCore import QPoint
from PySide6.QtWidgets import QWidget

from ...controllers.context_menu_controller import MenuAction
from .action_menu import ClipboardActionMenu


class ClipboardGroupContextMenu(ClipboardActionMenu):
    """根据 controller 产出的分组菜单数据渲染 QMenu。"""

    def show(self, anchor_widget: QWidget, pos: QPoint, actions: Sequence[MenuAction]):
        self.show_actions(anchor_widget, pos, actions)