# -*- coding: utf-8 -*-
"""
Emoji 数据助手单元测试

测试 emoji 分组加载、图标获取等纯逻辑。
"""
import pytest
from clipboard.emoji_data import get_emoji_groups, get_group_icon, GROUP_ICONS, _cached_groups, _cached_order
import clipboard.emoji_data as emoji_mod


class TestGroupIcons:
    """GROUP_ICONS 常量测试"""

    def test_group_icons_not_empty(self):
        """分组图标字典不应为空"""
        assert len(GROUP_ICONS) > 0

    def test_all_icons_are_emoji(self):
        """所有图标应为非空字符串"""
        for group, icon in GROUP_ICONS.items():
            assert isinstance(icon, str), f"{group} 的图标不是字符串"
            assert len(icon) > 0, f"{group} 的图标为空"

    def test_known_groups_exist(self):
        """已知分组应存在"""
        expected = ["Smileys & Emotion", "People & Body", "Animals & Nature"]
        for name in expected:
            assert name in GROUP_ICONS


class TestGetGroupIcon:
    """get_group_icon 函数测试"""

    def test_known_group(self):
        """已知分组应返回对应图标"""
        assert get_group_icon("Smileys & Emotion") == "😀"
        assert get_group_icon("Animals & Nature") == "🐱"

    def test_unknown_group(self):
        """未知分组应返回默认图标"""
        assert get_group_icon("NonExistentGroup") == "📁"


class TestGetEmojiGroups:
    """get_emoji_groups 函数测试"""

    @pytest.fixture(autouse=True)
    def clear_cache(self):
        """清除缓存以确保真正加载"""
        emoji_mod._cached_order = []
        emoji_mod._cached_groups = {}
        yield

    def test_returns_tuple(self):
        """应返回二元组"""
        result = get_emoji_groups()
        assert isinstance(result, tuple)
        assert len(result) == 2

    def test_order_is_list(self):
        """第一个元素应为分组名列表"""
        order, groups = get_emoji_groups()
        assert isinstance(order, list)
        assert len(order) > 0
        for name in order:
            assert isinstance(name, str)

    def test_groups_is_dict(self):
        """第二个元素应为分组字典"""
        order, groups = get_emoji_groups()
        assert isinstance(groups, dict)
        assert len(groups) > 0

    def test_order_matches_groups(self):
        """分组顺序列表应与字典键匹配"""
        order, groups = get_emoji_groups()
        for name in order:
            assert name in groups, f"分组 '{name}' 在 order 中但不在 groups 字典中"

    def test_each_group_has_emojis(self):
        """每个分组应包含 emoji 列表"""
        order, groups = get_emoji_groups()
        for name, emojis in groups.items():
            assert isinstance(emojis, list), f"分组 '{name}' 的值不是列表"
            assert len(emojis) > 0, f"分组 '{name}' 没有 emoji"

    def test_caching(self):
        """第二次调用应使用缓存"""
        order1, groups1 = get_emoji_groups()
        order2, groups2 = get_emoji_groups()
        # 应返回同一个对象（缓存）
        assert order1 is order2
        assert groups1 is groups2
 