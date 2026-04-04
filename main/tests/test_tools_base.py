# -*- coding: utf-8 -*-
"""
Tool 基类和 ToolContext 单元测试

测试工具上下文创建、load_settings / save_settings 逻辑。
"""
import pytest
from PySide6.QtWidgets import QApplication
from PySide6.QtGui import QColor
from PySide6.QtCore import QPointF
from unittest.mock import MagicMock


@pytest.fixture(scope="module")
def qapp():
    app = QApplication.instance()
    if app is None:
        app = QApplication([])
    yield app


class TestToolContext:
    """ToolContext 数据类测试"""

    def test_create_context(self, qapp):
        """创建工具上下文"""
        from tools.base import ToolContext
        ctx = ToolContext(
            scene=MagicMock(),
            selection=MagicMock(),
            undo_stack=MagicMock(),
            color=QColor("#FF0000"),
            stroke_width=5,
            opacity=0.8,
        )
        assert ctx.color == QColor("#FF0000")
        assert ctx.stroke_width == 5
        assert ctx.opacity == 0.8
        assert ctx.settings_manager is None

    def test_context_with_settings_manager(self, qapp):
        """带设置管理器的上下文"""
        from tools.base import ToolContext
        mock_settings = MagicMock()
        ctx = ToolContext(
            scene=MagicMock(),
            selection=MagicMock(),
            undo_stack=MagicMock(),
            color=QColor(),
            stroke_width=3,
            opacity=1.0,
            settings_manager=mock_settings,
        )
        assert ctx.settings_manager is mock_settings


class TestToolBase:
    """Tool 基类测试"""

    @pytest.fixture
    def tool(self, qapp):
        from tools.base import Tool
        return Tool()

    @pytest.fixture
    def ctx(self, qapp):
        from tools.base import ToolContext
        return ToolContext(
            scene=MagicMock(),
            selection=MagicMock(),
            undo_stack=MagicMock(),
            color=QColor("#FF0000"),
            stroke_width=5,
            opacity=1.0,
        )

    def test_tool_id(self, tool):
        """基类工具 ID"""
        assert tool.id == "base"

    def test_on_press_noop(self, tool, ctx):
        """基类 on_press 不崩溃"""
        tool.on_press(QPointF(10, 20), None, ctx)

    def test_on_move_noop(self, tool, ctx):
        """基类 on_move 不崩溃"""
        tool.on_move(QPointF(10, 20), ctx)

    def test_on_release_noop(self, tool, ctx):
        """基类 on_release 不崩溃"""
        tool.on_release(QPointF(10, 20), ctx)

    def test_on_deactivate_noop(self, tool, ctx):
        """基类 on_deactivate 不崩溃（无 settings_manager）"""
        tool.on_deactivate(ctx)

    def test_load_settings_with_manager(self, tool, qapp):
        """load_settings 从 settings_manager 加载"""
        from tools.base import ToolContext
        mock_settings = MagicMock()
        mock_settings.get_color.return_value = QColor("#00FF00")
        mock_settings.get_stroke_width.return_value = 10
        mock_settings.get_opacity.return_value = 0.5

        ctx = ToolContext(
            scene=MagicMock(),
            selection=MagicMock(),
            undo_stack=MagicMock(),
            color=QColor("#FF0000"),
            stroke_width=5,
            opacity=1.0,
            settings_manager=mock_settings,
        )
        tool.load_settings(ctx)
        assert ctx.color == QColor("#00FF00")
        assert ctx.stroke_width == 10
        assert ctx.opacity == 0.5

    def test_load_settings_no_manager(self, tool, ctx):
        """无 settings_manager 时 load_settings 不崩溃"""
        original_color = QColor(ctx.color)
        tool.load_settings(ctx)
        assert ctx.color == original_color

    def test_save_settings_with_manager(self, tool, qapp):
        """save_settings 保存到 settings_manager"""
        from tools.base import ToolContext
        mock_settings = MagicMock()
        ctx = ToolContext(
            scene=MagicMock(),
            selection=MagicMock(),
            undo_stack=MagicMock(),
            color=QColor("#AABBCC"),
            stroke_width=8,
            opacity=0.7,
            settings_manager=mock_settings,
        )
        tool.save_settings(ctx)
        mock_settings.update_settings.assert_called_once()

    def test_color_with_opacity(self, qapp):
        """color_with_opacity 工具函数"""
        from tools.base import color_with_opacity
        c = color_with_opacity(QColor("#FF0000"), 0.5)
        assert abs(c.alphaF() - 0.5) < 0.01
        assert c.red() == 255

    def test_color_with_opacity_none(self, qapp):
        """opacity=None 时使用 1.0"""
        from tools.base import color_with_opacity
        c = color_with_opacity(QColor("#00FF00"), None)
        assert abs(c.alphaF() - 1.0) < 0.01

    def test_color_with_opacity_clamp(self, qapp):
        """opacity 超出范围时应被钳制"""
        from tools.base import color_with_opacity
        c1 = color_with_opacity(QColor("#0000FF"), -0.5)
        assert c1.alphaF() >= 0.0
        c2 = color_with_opacity(QColor("#0000FF"), 2.0)
        assert c2.alphaF() <= 1.0
 