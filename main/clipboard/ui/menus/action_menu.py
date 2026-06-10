# -*- coding: utf-8 -*-
"""剪贴板右键菜单通用 UI 构建。"""

from typing import Callable, Dict, Optional, Sequence

from PySide6.QtCore import QPoint
from PySide6.QtWidgets import QMenu, QWidget

from ...controllers.context_menu_controller import MenuAction


ActionHandler = Callable[[], None]
DynamicHandlerResolver = Callable[[str], Optional[ActionHandler]]


class ClipboardActionMenu:
    """根据 MenuAction 序列渲染 QMenu。"""

    def __init__(
        self,
        parent: QWidget,
        menu_style: str,
        translate: Callable[[str], str],
        action_handlers: Dict[str, ActionHandler],
        dynamic_handler_resolvers: Sequence[DynamicHandlerResolver] = (),
    ):
        self._parent = parent
        self._menu_style = menu_style
        self._translate = translate
        self._action_handlers = action_handlers
        self._dynamic_handler_resolvers = tuple(dynamic_handler_resolvers)

    def show_actions(self, anchor_widget: QWidget, pos: QPoint, actions: Sequence[MenuAction]):
        if not actions:
            return

        menu = QMenu(self._parent)
        menu.setStyleSheet(self._menu_style)
        self._populate_menu(menu, actions)
        menu.exec(anchor_widget.mapToGlobal(pos))

    def _populate_menu(self, menu: QMenu, actions: Sequence[MenuAction]):
        for action_data in actions:
            if action_data.is_separator:
                menu.addSeparator()
                continue

            if action_data.children:
                submenu = menu.addMenu(self._translate_label(action_data))
                submenu.setStyleSheet(self._menu_style)
                self._populate_menu(submenu, action_data.children)
                submenu.menuAction().setEnabled(action_data.enabled)
                continue

            action = menu.addAction(self._translate_label(action_data))
            action.setEnabled(action_data.enabled)
            if action_data.checkable:
                action.setCheckable(True)
                action.setChecked(action_data.checked)

            handler = self._resolve_handler(action_data.key)
            if handler is not None:
                action.triggered.connect(handler)

    def _resolve_handler(self, action_key: str) -> Optional[ActionHandler]:
        handler = self._action_handlers.get(action_key)
        if handler is not None:
            return handler

        for resolver in self._dynamic_handler_resolvers:
            handler = resolver(action_key)
            if handler is not None:
                return handler

        return None

    def _translate_label(self, action_data: MenuAction) -> str:
        if not action_data.translate_label:
            return action_data.label
        return self._translate(action_data.label)