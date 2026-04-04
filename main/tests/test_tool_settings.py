# -*- coding: utf-8 -*-
"""
ToolSettings / ToolSettingsManager 单元测试

测试工具设置的增删改查、持久化、重置逻辑。
需要 QApplication 实例（因为 QSettings / Signal 依赖 Qt）。
"""
import pytest
import tempfile
import os
from PySide6.QtWidgets import QApplication
from PySide6.QtCore import QSettings
from settings.tool_settings import ToolSettings, ToolSettingsManager


@pytest.fixture(scope="module")
def qapp():
    """模块级 QApplication"""
    app = QApplication.instance()
    if app is None:
        app = QApplication([])
    yield app


# ==================== ToolSettings 测试 ====================

class TestToolSettings:
    """单个工具设置数据类测试"""

    def test_get_default(self):
        """获取默认值"""
        ts = ToolSettings("pen", {"color": "#FF0000", "width": 3})
        assert ts.get("color") == "#FF0000"
        assert ts.get("width") == 3

    def test_get_missing_key(self):
        """获取不存在的键应返回 None 或默认值"""
        ts = ToolSettings("pen", {"color": "#FF0000"})
        assert ts.get("nonexistent") is None
        assert ts.get("nonexistent", "fallback") == "fallback"

    def test_set_value(self):
        """设置值"""
        ts = ToolSettings("pen", {"color": "#FF0000"})
        ts.set("color", "#00FF00")
        assert ts.get("color") == "#00FF00"

    def test_update_batch(self):
        """批量更新"""
        ts = ToolSettings("pen", {"color": "#FF0000", "width": 3})
        ts.update(color="#0000FF", width=5)
        assert ts.get("color") == "#0000FF"
        assert ts.get("width") == 5

    def test_reset_to_defaults(self):
        """重置回默认值"""
        ts = ToolSettings("pen", {"color": "#FF0000", "width": 3})
        ts.set("color", "#FFFFFF")
        ts.set("width", 99)
        ts.reset_to_defaults()
        assert ts.get("color") == "#FF0000"
        assert ts.get("width") == 3

    def test_to_dict(self):
        """导出为字典"""
        ts = ToolSettings("pen", {"color": "#FF0000", "width": 3})
        d = ts.to_dict()
        assert d == {"color": "#FF0000", "width": 3}

    def test_from_dict(self):
        """从字典导入"""
        ts = ToolSettings("pen", {"color": "#FF0000", "width": 3})
        ts.from_dict({"color": "#ABCDEF", "width": 10})
        assert ts.get("color") == "#ABCDEF"
        assert ts.get("width") == 10

    def test_tool_id(self):
        """工具 ID 应正确存储"""
        ts = ToolSettings("arrow", {})
        assert ts.tool_id == "arrow"


# ==================== ToolSettingsManager 测试 ====================

class TestToolSettingsManager:
    """设置管理器测试"""

    @pytest.fixture
    def manager(self, qapp, tmp_path):
        """创建一个使用临时文件存储的管理器实例（与系统注册表隔离）"""
        ini_path = str(tmp_path / "test_settings.ini")
        qs = QSettings(ini_path, QSettings.Format.IniFormat)
        mgr = ToolSettingsManager(qsettings=qs)
        yield mgr

    def test_default_tools_initialized(self, manager):
        """所有默认工具都应被初始化"""
        expected_tools = ["pen", "highlighter", "rect", "ellipse", "arrow", "text", "number", "eraser"]
        for tool_id in expected_tools:
            assert manager.get_tool_settings(tool_id) is not None, f"工具 {tool_id} 未初始化"

    def test_get_setting(self, manager):
        """获取工具的设置值"""
        color = manager.get_setting("pen", "color")
        # 值可能被之前的测试/用户修改过，只验证返回了有效颜色字符串
        assert isinstance(color, str)
        assert color.startswith("#")

    def test_set_setting(self, manager):
        """设置工具的值"""
        manager.set_setting("pen", "color", "#00FF00")
        assert manager.get_setting("pen", "color") == "#00FF00"
        # 恢复
        manager.set_setting("pen", "color", "#FF0000")

    def test_get_nonexistent_tool(self, manager):
        """访问不存在的工具应返回 None"""
        assert manager.get_tool_settings("nonexistent") is None
        assert manager.get_setting("nonexistent", "color") is None

    def test_reset_tool(self, manager):
        """重置单个工具应恢复为 DEFAULT_SETTINGS 中的默认值"""
        default_width = ToolSettingsManager.DEFAULT_SETTINGS["pen"]["stroke_width"]
        manager.set_setting("pen", "stroke_width", 999)
        manager.reset_tool("pen")
        assert manager.get_setting("pen", "stroke_width") == default_width

    def test_update_settings_batch(self, manager):
        """批量更新工具设置"""
        manager.update_settings("rect", color="#123456", stroke_width=7)
        assert manager.get_setting("rect", "color") == "#123456"
        assert manager.get_setting("rect", "stroke_width") == 7
        # 恢复
        manager.reset_tool("rect")

    def test_get_app_setting_default(self, manager):
        """获取应用级设置的默认值"""
        hotkey = manager.get_app_setting("hotkey")
        assert hotkey is not None

    def test_set_app_setting(self, manager):
        """设置应用级设置"""
        manager.set_app_setting("log_level", "DEBUG")
        assert manager.get_app_setting("log_level") == "DEBUG"
        # 恢复
        manager.set_app_setting("log_level", "INFO")

    def test_get_color(self, manager):
        """获取 QColor 对象"""
        from PySide6.QtGui import QColor
        color = manager.get_color("pen")
        assert isinstance(color, QColor)
        assert color.isValid()

    def test_get_stroke_width(self, manager):
        """获取笔触宽度"""
        width = manager.get_stroke_width("pen")
        assert isinstance(width, int)
        assert width > 0

    def test_get_opacity(self, manager):
        """获取透明度"""
        opacity = manager.get_opacity("pen")
        assert isinstance(opacity, float)
        assert 0.0 <= opacity <= 1.0

    def test_app_default_settings_keys(self, manager):
        """APP_DEFAULT_SETTINGS 应包含关键配置"""
        defaults = ToolSettingsManager.APP_DEFAULT_SETTINGS
        assert "hotkey" in defaults
        assert "language" in defaults
        assert "clipboard_enabled" in defaults
        assert "ocr_enabled" in defaults
 