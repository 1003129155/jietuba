# -*- coding: utf-8 -*-
"""剪贴板右键菜单控制器。"""

from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Callable, List, Optional, Tuple

from ..core import ClipboardItem, Group, GroupType

if TYPE_CHECKING:
    from .clipboard_controller import ClipboardController


@dataclass
class MenuAction:
    """单个菜单动作描述。"""

    label: str
    key: str
    enabled: bool = True
    checkable: bool = False
    checked: bool = False
    is_separator: bool = False
    children: List["MenuAction"] = field(default_factory=list)
    translate_label: bool = True


@dataclass
class ContextMenuData:
    """内容右键菜单完整数据。"""

    item_id: int
    actions: List[MenuAction]


@dataclass
class _ItemContextMenuState:
    """内容右键菜单构建时使用的上下文状态。"""

    clipboard_item: ClipboardItem
    current_group_id: Optional[int]
    visible_groups: List[Group]
    can_move_up: bool = False
    can_move_down: bool = False

    @property
    def is_image_item(self) -> bool:
        return self.clipboard_item.content_type == "image"

    @property
    def is_file_item(self) -> bool:
        return self.clipboard_item.content_type == "file"

    @property
    def in_group(self) -> bool:
        return self.current_group_id is not None


@dataclass(frozen=True)
class _ItemContextMenuSpec:
    """内容右键菜单动作规则。"""

    key: str
    label: str = ""
    is_separator: bool = False
    predicate: Callable[["_ItemContextMenuState"], bool] = lambda _state: True
    label_builder: Optional[Callable[["_ItemContextMenuState"], str]] = None
    enabled_builder: Optional[Callable[["_ItemContextMenuState"], bool]] = None
    children_builder: Optional[Callable[["_ItemContextMenuState"], List[MenuAction]]] = None
    translate_label: bool = True


@dataclass(frozen=True)
class _GroupContextMenuState:
    """分组右键菜单构建时使用的上下文状态。"""

    can_move_up: bool
    can_move_down: bool


@dataclass(frozen=True)
class _GroupContextMenuSpec:
    """分组右键菜单动作规则。"""

    key: str
    label: str = ""
    is_separator: bool = False
    predicate: Callable[["_GroupContextMenuState"], bool] = lambda _state: True
    label_builder: Optional[Callable[["_GroupContextMenuState"], str]] = None
    enabled_builder: Optional[Callable[["_GroupContextMenuState"], bool]] = None
    translate_label: bool = True


def _normalize_menu_actions(actions: List[MenuAction]) -> List[MenuAction]:
    """移除首尾/重复分隔线，并清理空子菜单。"""

    normalized: List[MenuAction] = []

    for action in actions:
        if action.children:
            action.children = _normalize_menu_actions(action.children)
            if not action.children and not action.is_separator:
                continue

        if action.is_separator and (not normalized or normalized[-1].is_separator):
            continue

        normalized.append(action)

    if normalized and normalized[-1].is_separator:
        normalized.pop()

    return normalized


def _build_move_group_menu_children(state: _ItemContextMenuState) -> List[MenuAction]:
    children = [
        MenuAction(
            label=f"{group.icon} {group.name}" if group.icon else group.name,
            key=f"move_to_group_{group.id}",
            translate_label=False,
        )
        for group in state.visible_groups
    ]

    if state.in_group:
        children.append(MenuAction(label="", key="sep_remove_from_group", is_separator=True))
        children.append(MenuAction(label="Remove from Group", key="remove_from_group"))

    return _normalize_menu_actions(children)


def _build_special_paste_menu_children(state: _ItemContextMenuState) -> List[MenuAction]:
    """构建"特殊粘贴"子菜单项列表。"""
    if _is_text_item(state):
        children: List[MenuAction] = [
            # 1. 粘贴纯文本（等价于当前不带格式粘贴）
            MenuAction(label="Paste Plain Text", key="special_paste_plain_text"),
            MenuAction(label="", key="sep_sp_1", is_separator=True),
            # 2-6. 大小写转换
            MenuAction(label="All Uppercase", key="transform_uppercase"),
            MenuAction(label="All Lowercase", key="transform_lowercase"),
            MenuAction(label="Capitalize Words", key="transform_capitalize_words"),
            MenuAction(label="Capitalize Sentences", key="transform_capitalize_sentences"),
            MenuAction(label="Toggle Case", key="transform_toggle_case"),
            MenuAction(label="", key="sep_sp_2", is_separator=True),
            # 7. SQL IN 句
            MenuAction(label="SQL IN Clause", key="transform_sql_in"),
            MenuAction(label="", key="sep_sp_3", is_separator=True),
            # 8. 移除换行符
            MenuAction(label="Remove Line Breaks", key="transform_remove_linebreaks"),
            # 9. 粘贴并添加当前时间
            MenuAction(label="Paste with Current Time", key="transform_append_time"),
            MenuAction(label="", key="sep_sp_4", is_separator=True),
            # 10. 保持顺序粘贴（不把该项移到最前）
            MenuAction(label="Paste in Order", key="special_paste_in_order"),
        ]
        return _normalize_menu_actions(children)

    if state.is_file_item:
        return [
            MenuAction(label="File Name", key="file_paste_names"),
            MenuAction(label="File Link", key="file_paste_links"),
        ]

    return []


def _is_text_item(state: _ItemContextMenuState) -> bool:
    """判断是否为文本类型项目。"""
    return state.clipboard_item.content_type == "text"


_ITEM_CONTEXT_MENU_SPECS: Tuple[_ItemContextMenuSpec, ...] = (
    _ItemContextMenuSpec(key="paste", label="Paste"),
    _ItemContextMenuSpec(
        key="special_paste_menu",
        label="Special Paste",
        predicate=lambda state: _is_text_item(state) or state.is_file_item,
        children_builder=_build_special_paste_menu_children,
    ),
    _ItemContextMenuSpec(key="sep_after_paste", is_separator=True),
    _ItemContextMenuSpec(key="pin_image", label="Pin as Overlay", predicate=lambda state: state.is_image_item),
    _ItemContextMenuSpec(key="save_image_as", label="Save as", predicate=lambda state: state.is_image_item),
    _ItemContextMenuSpec(
        key="toggle_pin",
        predicate=lambda state: not state.is_image_item,
        label_builder=lambda state: "Unpin" if state.clipboard_item.is_pinned else "Pin",
    ),
    _ItemContextMenuSpec(
        key="open_file_location",
        label="Open File Location",
        predicate=lambda state: state.is_file_item,
    ),
    _ItemContextMenuSpec(key="sep_before_groups", is_separator=True),
    _ItemContextMenuSpec(
        key="move_group_menu",
        label="Move to Group",
        predicate=lambda state: bool(state.visible_groups),
        children_builder=_build_move_group_menu_children,
    ),
    _ItemContextMenuSpec(key="sep_before_order", is_separator=True),
    _ItemContextMenuSpec(key="edit_item", label="Edit", predicate=lambda state: state.in_group),
    _ItemContextMenuSpec(
        key="move_item_up",
        label="Move Up",
        predicate=lambda state: state.in_group,
        enabled_builder=lambda state: state.can_move_up,
    ),
    _ItemContextMenuSpec(
        key="move_item_down",
        label="Move Down",
        predicate=lambda state: state.in_group,
        enabled_builder=lambda state: state.can_move_down,
    ),
    _ItemContextMenuSpec(key="sep_before_delete", is_separator=True),
    _ItemContextMenuSpec(key="delete_item", label="Delete"),
)


_GROUP_CONTEXT_MENU_SPECS: Tuple[_GroupContextMenuSpec, ...] = (
    _GroupContextMenuSpec(key="edit_group", label="Edit"),
    _GroupContextMenuSpec(
        key="move_group_up",
        label="Move Up",
        enabled_builder=lambda state: state.can_move_up,
    ),
    _GroupContextMenuSpec(
        key="move_group_down",
        label="Move Down",
        enabled_builder=lambda state: state.can_move_down,
    ),
    _GroupContextMenuSpec(key="sep_before_delete", is_separator=True),
    _GroupContextMenuSpec(key="delete_group", label="Delete Group"),
)


class ClipboardContextMenuController:
    """从 ClipboardController 中抽离出的右键菜单规则控制器。"""

    def __init__(self, controller: "ClipboardController"):
        self._controller = controller

    def build_context_menu_data(self, item_id: int) -> Optional[ContextMenuData]:
        """组装内容右键菜单所需数据。返回 None 表示 item_id 无效。"""
        clipboard_item = self._controller.get_item(item_id)
        if clipboard_item is None:
            return None

        state = self._build_item_context_menu_state(clipboard_item)
        actions = self._build_item_context_menu_actions(state)
        return ContextMenuData(item_id=item_id, actions=actions)

    def build_group_context_menu_data(self, group_id: int) -> List[MenuAction]:
        """组装分组右键菜单数据。"""
        state = self._build_group_context_menu_state(group_id)
        return self._build_group_context_menu_actions(state)

    def _get_item_context_menu_groups(self, clipboard_item: ClipboardItem) -> List[Group]:
        is_file_item = clipboard_item.content_type == "file"
        return [
            group
            for group in self._controller.get_groups()
            if group.group_type != GroupType.HIDDEN and (group.group_type == GroupType.NORMAL or is_file_item)
        ]

    def _build_item_context_menu_state(self, clipboard_item: ClipboardItem) -> _ItemContextMenuState:
        can_up, can_down = False, False
        current_group_id = self._controller.current_group_id
        if current_group_id is not None:
            can_up, can_down = self._controller.get_item_move_state(clipboard_item.id, current_group_id)

        return _ItemContextMenuState(
            clipboard_item=clipboard_item,
            current_group_id=current_group_id,
            visible_groups=self._get_item_context_menu_groups(clipboard_item),
            can_move_up=can_up,
            can_move_down=can_down,
        )

    def _build_item_context_menu_action(
        self, spec: _ItemContextMenuSpec, state: _ItemContextMenuState
    ) -> Optional[MenuAction]:
        if not spec.predicate(state):
            return None

        if spec.is_separator:
            return MenuAction(label="", key=spec.key, is_separator=True)

        children = spec.children_builder(state) if spec.children_builder is not None else []
        if spec.children_builder is not None and not children:
            return None

        return MenuAction(
            label=spec.label_builder(state) if spec.label_builder is not None else spec.label,
            key=spec.key,
            enabled=spec.enabled_builder(state) if spec.enabled_builder is not None else True,
            children=children,
            translate_label=spec.translate_label,
        )

    def _build_item_context_menu_actions(self, state: _ItemContextMenuState) -> List[MenuAction]:
        actions: List[MenuAction] = []
        for spec in _ITEM_CONTEXT_MENU_SPECS:
            action = self._build_item_context_menu_action(spec, state)
            if action is not None:
                actions.append(action)
        return _normalize_menu_actions(actions)

    def _build_group_context_menu_state(self, group_id: int) -> _GroupContextMenuState:
        can_up, can_down = self._controller.get_group_move_state(group_id)
        return _GroupContextMenuState(can_move_up=can_up, can_move_down=can_down)

    def _build_group_context_menu_action(
        self, spec: _GroupContextMenuSpec, state: _GroupContextMenuState
    ) -> Optional[MenuAction]:
        if not spec.predicate(state):
            return None

        if spec.is_separator:
            return MenuAction(label="", key=spec.key, is_separator=True)

        return MenuAction(
            label=spec.label_builder(state) if spec.label_builder is not None else spec.label,
            key=spec.key,
            enabled=spec.enabled_builder(state) if spec.enabled_builder is not None else True,
            translate_label=spec.translate_label,
        )

    def _build_group_context_menu_actions(self, state: _GroupContextMenuState) -> List[MenuAction]:
        actions: List[MenuAction] = []
        for spec in _GROUP_CONTEXT_MENU_SPECS:
            action = self._build_group_context_menu_action(spec, state)
            if action is not None:
                actions.append(action)
        return _normalize_menu_actions(actions)


__all__ = ["ClipboardContextMenuController", "ContextMenuData", "MenuAction"]
