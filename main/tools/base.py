"""
工具基类和上下文
"""

from dataclasses import dataclass
from typing import Optional

from PyQt6.QtGui import QColor
from PyQt6.QtCore import QPointF


@dataclass
class ToolContext:
    """
    工具上下文 - 包含工具所需的所有依赖
    """
    scene: object          # CanvasScene
    selection: object      # SelectionModel
    undo_stack: object     # CommandUndoStack (原 undo)
    color: QColor          # 当前颜色
    stroke_width: int      # 笔触宽度
    opacity: float         # 透明度 (0.0-1.0)
    settings_manager: object = None  # ToolSettingsManager（新增）



class Tool:
    """
    工具基类 - 所有绘图工具的父类
    """
    
    id = "base"  # 工具ID（子类必须重写）
    
    def on_press(self, pos: QPointF, button, ctx: ToolContext):
        """
        鼠标按下事件
        
        Args:
            pos: 鼠标位置（场景坐标）
            button: 鼠标按钮
            ctx: 工具上下文
        """
        pass
    
    def on_move(self, pos: QPointF, ctx: ToolContext):
        """
        鼠标移动事件
        
        Args:
            pos: 鼠标位置（场景坐标）
            ctx: 工具上下文
        """
        pass
    
    def on_release(self, pos: QPointF, ctx: ToolContext):
        """
        鼠标释放事件
        
        Args:
            pos: 鼠标位置（场景坐标）
            ctx: 工具上下文
        """
        pass
    
    def on_activate(self, ctx: ToolContext):
        """工具激活时调用 - 先加载设置，再设置光标"""
        # 先加载工具的设置到上下文中
        if ctx.settings_manager:
            self.load_settings(ctx)
        
        # 然后设置工具光标
        if hasattr(ctx.scene, 'cursor_manager') and ctx.scene.cursor_manager:
            ctx.scene.cursor_manager.set_tool_cursor(self.id)
        # 兼容旧代码：尝试从 canvas_widget 获取
        elif hasattr(ctx, 'canvas_widget') and hasattr(ctx.canvas_widget, 'cursor_manager'):
            ctx.canvas_widget.cursor_manager.set_tool_cursor(self.id)
    
    def on_deactivate(self, ctx: ToolContext):
        """
        工具停用时调用
        
        Args:
            ctx: 工具上下文
        """
        # 如果有设置管理器，保存当前工具的设置
        if ctx.settings_manager:
            self.save_settings(ctx)
    
    def load_settings(self, ctx: ToolContext):
        """
        从设置管理器加载工具设置到上下文
        
        Args:
            ctx: 工具上下文
        """
        if not ctx.settings_manager:
            return
        
        # 加载颜色
        color = ctx.settings_manager.get_color(self.id)
        if color.isValid():
            ctx.color = color
        
        # 加载笔触宽度
        stroke_width = ctx.settings_manager.get_stroke_width(self.id)
        if stroke_width:
            ctx.stroke_width = stroke_width
        
        # 加载透明度
        opacity = ctx.settings_manager.get_opacity(self.id)
        if opacity is not None:
            ctx.opacity = opacity
    
    def save_settings(self, ctx: ToolContext):
        """
        将当前上下文的设置保存到设置管理器
        
        Args:
            ctx: 工具上下文
        """
        if not ctx.settings_manager:
            return
        
        # 批量保存设置
        ctx.settings_manager.update_settings(
            self.id,
            save_immediately=True,
            color=ctx.color.name(),
            stroke_width=ctx.stroke_width,
            opacity=ctx.opacity
        )


def color_with_opacity(source: QColor, opacity: Optional[float]) -> QColor:
    """返回应用透明度后的颜色副本"""
    color = QColor(source)
    if opacity is None:
        opacity = 1.0
    opacity = max(0.0, min(1.0, float(opacity)))
    color.setAlphaF(opacity)
    return color

