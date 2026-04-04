# -*- coding: utf-8 -*-
"""
emoji_data.py — 从预生成的 emoji_groups.json 加载 emoji 分组数据

数据文件位于 svg/emoji_groups.json，已按分组过滤好
（≤E13.1、fully-qualified、无肤色/发型变体、无 Component/Flags 组）。
"""

import json
from pathlib import Path
from typing import Dict, List, Tuple

from core.resource_manager import ResourceManager

# 内存缓存
_cached_order: List[str] = []
_cached_groups: Dict[str, List[str]] = {}

# 每个分组的代表 emoji（用于 tab 图标）
GROUP_ICONS = {
    "Smileys & Emotion": "😀",
    "People & Body":     "👋",
    "Animals & Nature":  "🐱",
    "Food & Drink":      "🍎",
    "Travel & Places":   "✈️",
    "Activities":        "⚽",
    "Objects":           "💡",
    "Symbols":           "❤️",
}


def _json_path() -> Path:
    """emoji_groups.json 的路径（兼容开发环境和打包后）"""
    return Path(ResourceManager.get_resource_path("svg/emoji_groups.json"))


def get_emoji_groups() -> Tuple[List[str], Dict[str, List[str]]]:
    """
    获取 emoji 分组数据（带内存缓存）。
    返回 (group_order, groups_dict)。
    """
    global _cached_order, _cached_groups
    if _cached_groups:
        return _cached_order, _cached_groups

    with open(_json_path(), "r", encoding="utf-8") as f:
        data = json.load(f)

    _cached_order = data["order"]
    _cached_groups = data["groups"]
    return _cached_order, _cached_groups


def get_group_icon(group_name: str) -> str:
    """获取分组的代表图标"""
    return GROUP_ICONS.get(group_name, "📁")
 