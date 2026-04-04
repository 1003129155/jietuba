# -*- coding: utf-8 -*-
"""
ResourceManager 资源路径管理器单元测试

测试路径解析和图标缓存逻辑。
"""
import pytest
import os
from core.resource_manager import ResourceManager


class TestResourceManager:
    """ResourceManager 静态方法测试"""

    def test_get_resource_path_returns_string(self):
        """应返回字符串路径"""
        path = ResourceManager.get_resource_path("svg/some_icon.svg")
        assert isinstance(path, str)

    def test_get_resource_path_absolute(self):
        """返回的路径应为绝对路径"""
        path = ResourceManager.get_resource_path("svg/test.svg")
        assert os.path.isabs(path)

    def test_get_resource_path_contains_relative(self):
        """返回路径应包含传入的相对路径"""
        path = ResourceManager.get_resource_path("svg/fluent_icons/arrow.svg")
        assert "svg" in path
        assert "fluent_icons" in path
        assert "arrow.svg" in path

    def test_get_icon_path(self):
        """get_icon_path 应等同于 get_resource_path('svg/' + name)"""
        icon_path = ResourceManager.get_icon_path("test_icon.svg")
        direct_path = ResourceManager.get_resource_path(os.path.join("svg", "test_icon.svg"))
        assert icon_path == direct_path

    def test_get_icon_path_returns_string(self):
        """应返回字符串"""
        path = ResourceManager.get_icon_path("icon.svg")
        assert isinstance(path, str)

    def test_get_resource_path_empty_string(self):
        """空字符串不崩溃"""
        path = ResourceManager.get_resource_path("")
        assert isinstance(path, str)

    def test_actual_svg_dir_exists(self):
        """svg 目录应存在"""
        svg_dir = ResourceManager.get_resource_path("svg")
        assert os.path.isdir(svg_dir), f"svg 目录不存在: {svg_dir}"

    def test_emoji_groups_json_exists(self):
        """emoji_groups.json 应存在"""
        path = ResourceManager.get_resource_path("svg/emoji_groups.json")
        assert os.path.isfile(path), f"文件不存在: {path}"
 