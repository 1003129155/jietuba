# -*- coding: utf-8 -*-

"""分组辅助服务。

集中处理分组默认图标、名称冲突判断、唯一命名和删除确认文案，
供管理窗口、表单层和业务服务层复用。
"""

from typing import Any, Callable, Iterable


GENERAL_GROUP_ICON = "📁"
QUICK_LAUNCH_GROUP_ICON = "⚡"


def get_default_group_icon(is_file_group: bool) -> str:
    """返回分组类型对应的默认图标。"""
    return QUICK_LAUNCH_GROUP_ICON if is_file_group else GENERAL_GROUP_ICON


def get_group_display_icon(custom_icon: str | None, is_file_group: bool) -> str:
    """返回分组显示图标，优先使用自定义图标。"""
    return custom_icon or get_default_group_icon(is_file_group)


def get_toggled_default_group_icon(current_icon: str, is_file_group: bool) -> str:
    """根据分组类型切换默认图标，但不覆盖用户自定义图标。"""
    if is_file_group and current_icon == GENERAL_GROUP_ICON:
        return QUICK_LAUNCH_GROUP_ICON
    if not is_file_group and current_icon == QUICK_LAUNCH_GROUP_ICON:
        return GENERAL_GROUP_ICON
    return current_icon


def group_name_exists(groups: Iterable[Any], name: str, exclude_group_id: int | None = None) -> bool:
    """判断分组名称是否已存在。"""
    for group in groups:
        if exclude_group_id is not None and getattr(group, "id", None) == exclude_group_id:
            continue
        if getattr(group, "name", None) == name:
            return True
    return False


def make_unique_group_name(base_name: str, used_names: set[str]) -> str:
    """生成不重复的分组名称。"""
    if base_name not in used_names:
        return base_name

    index = 1
    while True:
        candidate = f"{base_name} ({index})"
        if candidate not in used_names:
            return candidate
        index += 1


def build_delete_group_confirm_message(translate: Callable[[str], str]) -> str:
    """构建删除分组确认文案。"""
    return "\n".join([
        translate("Are you sure you want to delete this group?"),
        translate("All items in the group will also be deleted."),
    ])
