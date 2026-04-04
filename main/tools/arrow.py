"""
箭头工具
"""

from PySide6.QtCore import QPointF, Qt
from PySide6.QtGui import QPen
from .base import Tool, ToolContext, color_with_opacity
from canvas.items import ArrowItem
from canvas.undo import AddItemCommand
from core import log_debug


class ArrowTool(Tool):
    """
    箭头工具
    """
    
    id = "arrow"
    
    # 最小绘制长度（像素），小于此值的绘制将被忽略
    MIN_LENGTH = 10
    
    def __init__(self):
        self.drawing = False
        self.start_pos = None
        self.current_item = None
        # 箭头样式：single（单头）或 double（双头）
        self.arrow_style = "single"
    
    def on_press(self, pos: QPointF, button, ctx: ToolContext):
        if button == Qt.MouseButton.LeftButton:
            self.drawing = True
            self.start_pos = pos
            
            # 从设置管理器获取箭头样式
            if ctx.settings_manager:
                settings = ctx.settings_manager.get_tool_settings("arrow")
                self.arrow_style = settings.get("arrow_style", "single")
            
            pen_color = color_with_opacity(ctx.color, ctx.opacity)
            pen = QPen(pen_color, ctx.stroke_width)
            self.current_item = ArrowItem(pos, pos, pen, self.arrow_style)
            ctx.scene.addItem(self.current_item)
            
            log_debug(f"开始绘制: {pos}, 样式: {self.arrow_style}", "ArrowTool")
    
    def on_move(self, pos: QPointF, ctx: ToolContext):
        if self.drawing and self.current_item:
            self.current_item.set_positions(self.start_pos, pos)
    
    def on_release(self, pos: QPointF, ctx: ToolContext):
        if self.drawing:
            self.drawing = False
            
            if self.current_item:
                # 检查箭头长度是否满足最小要求
                delta = pos - self.start_pos
                length = (delta.x() ** 2 + delta.y() ** 2) ** 0.5
                
                if length < self.MIN_LENGTH:
                    # 长度过短，取消绘制
                    ctx.scene.removeItem(self.current_item)
                    self.current_item = None
                    log_debug(f"绘制取消：长度过短 ({length:.1f} < {self.MIN_LENGTH})", "ArrowTool")
                    return
                
                ctx.scene.removeItem(self.current_item)
                command = AddItemCommand(ctx.scene, self.current_item)
                ctx.undo_stack.push(command)
                
                # 绘制完成后自动选择（方便调整）
                item_to_select = self.current_item
                self.current_item = None
                
                # 通过信号通知自动选中
                ctx.scene.item_auto_select_requested.emit(item_to_select)
            
            log_debug("完成绘制", "ArrowTool")
 