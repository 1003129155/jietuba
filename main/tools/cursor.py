"""
光标工具 (无工具模式)
作为默认工具的占位符，实际的选择和编辑功能由 SmartEditController 处理

说明：
- 此工具不执行任何绘制操作
- 选择/移动/编辑已绘制图元的功能由 SmartEditController 统一管理
- 仅作为"无工具激活"状态的标识，让用户可以与已绘制内容交互
"""

from PyQt6.QtCore import QPointF
from .base import Tool, ToolContext


class CursorTool(Tool):
    """
    光标工具（默认无绘制工具）
    
    功能：占位符工具，表示无绘制工具激活状态
    交互：所有选择/编辑功能由 SmartEditController 统一处理
    """
    
    id = "cursor"
    
    def __init__(self):
        super().__init__()
    
    def on_press(self, pos: QPointF, button, ctx: ToolContext):
        """鼠标按下 - 交由 SmartEditController 处理"""
        pass
    
    def on_move(self, pos: QPointF, ctx: ToolContext):
        """鼠标移动 - 交由 SmartEditController 处理"""
        pass
    
    def on_release(self, pos: QPointF, ctx: ToolContext):
        """鼠标松开 - 交由 SmartEditController 处理"""
        pass
