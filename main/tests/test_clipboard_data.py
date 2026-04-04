# -*- coding: utf-8 -*-
"""
ClipboardItem / Group 数据模型单元测试

测试 display_text / icon 等纯逻辑属性（不依赖 Rust 后端）。
"""
import pytest
import json
from datetime import datetime
from clipboard.data_manager import ClipboardItem, Group


# ==================== ClipboardItem 测试 ====================

class TestClipboardItem:
    """ClipboardItem 数据类测试"""

    def test_create_text_item(self):
        """创建文本类型剪贴板项"""
        item = ClipboardItem(
            id=1,
            content="Hello World",
            content_type="text",
        )
        assert item.id == 1
        assert item.content == "Hello World"
        assert item.content_type == "text"
        assert item.is_pinned is False
        assert item.paste_count == 0

    def test_display_text_short(self):
        """短文本直接显示"""
        item = ClipboardItem(id=1, content="短文本", content_type="text")
        assert item.display_text == "短文本"

    def test_display_text_truncate(self):
        """超过100字符的文本应截断"""
        long_text = "a" * 150
        item = ClipboardItem(id=1, content=long_text, content_type="text")
        assert item.display_text.endswith("...")
        assert len(item.display_text) == 103  # 100 + "..."

    def test_display_text_newline_replaced(self):
        """文本中的换行符应被替换为空格"""
        item = ClipboardItem(id=1, content="line1\nline2\nline3", content_type="text")
        assert "\n" not in item.display_text

    def test_display_text_with_title(self):
        """有标题时优先显示标题"""
        item = ClipboardItem(
            id=1, content="actual content", content_type="text", title="My Title"
        )
        assert item.display_text == "My Title"

    def test_display_text_image(self):
        """图片类型显示去掉方括号"""
        item = ClipboardItem(id=1, content="[图片 800x600]", content_type="image")
        assert item.display_text == "图片 800x600"

    def test_display_text_single_file(self):
        """单文件显示文件名"""
        files_data = json.dumps({"files": [r"C:\Users\test\doc.txt"]})
        item = ClipboardItem(id=1, content=files_data, content_type="file")
        assert item.display_text == "file: doc.txt"

    def test_display_text_multiple_files(self):
        """多文件显示完整路径"""
        files_data = json.dumps({"files": [r"C:\a.txt", r"C:\b.txt"]})
        item = ClipboardItem(id=1, content=files_data, content_type="file")
        assert "a.txt" in item.display_text
        assert "b.txt" in item.display_text

    def test_display_text_empty_files(self):
        """空文件列表"""
        files_data = json.dumps({"files": []})
        item = ClipboardItem(id=1, content=files_data, content_type="file")
        assert item.display_text == "file: 文件"

    def test_icon_text(self):
        """文本类型无图标"""
        item = ClipboardItem(id=1, content="text", content_type="text")
        assert item.icon == ""

    def test_icon_image(self):
        """图片类型有相机图标"""
        item = ClipboardItem(id=1, content="img", content_type="image")
        assert item.icon == "📷"

    def test_icon_file(self):
        """文件类型有文件夹图标"""
        item = ClipboardItem(id=1, content="file", content_type="file")
        assert item.icon == "📁"

    def test_optional_fields_default(self):
        """可选字段默认值"""
        item = ClipboardItem(id=1, content="c", content_type="text")
        assert item.title is None
        assert item.html_content is None
        assert item.image_id is None
        assert item.thumbnail is None
        assert item.source_app is None
        assert item.created_at is None
        assert item.updated_at is None


# ==================== Group 测试 ====================

class TestGroup:
    """Group 数据类测试"""

    def test_create_group(self):
        """创建分组"""
        group = Group(id=1, name="工作")
        assert group.id == 1
        assert group.name == "工作"
        assert group.color is None
        assert group.icon is None

    def test_group_with_color(self):
        """带颜色的分组"""
        group = Group(id=2, name="学习", color="#FF0000", icon="📚")
        assert group.color == "#FF0000"
        assert group.icon == "📚"
 