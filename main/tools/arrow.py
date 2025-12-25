"""
箭头工具
"""

from PyQt6.QtCore import QPointF, Qt
from PyQt6.QtGui import QPen
from .base import Tool, ToolContext, color_with_opacity
from canvas.items import ArrowItem
from canvas.undo import AddItemCommand


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
    
    def on_press(self, pos: QPointF, button, ctx: ToolContext):
        if button == Qt.MouseButton.LeftButton:
            self.drawing = True
            self.start_pos = pos
            
            pen_color = color_with_opacity(ctx.color, ctx.opacity)
            pen = QPen(pen_color, ctx.stroke_width)
            self.current_item = ArrowItem(pos, pos, pen)
            ctx.scene.addItem(self.current_item)
            
            print(f"[ArrowTool] 开始绘制: {pos}")
    
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
                    print(f"[ArrowTool] 绘制取消：长度过短 ({length:.1f} < {self.MIN_LENGTH})")
                    return
                
                ctx.scene.removeItem(self.current_item)
                command = AddItemCommand(ctx.scene, self.current_item)
                ctx.undo_stack.push(command)
                
                # 绘制完成后自动选择（方便调整）
                item_to_select = self.current_item
                self.current_item = None
                
                # 通知场景的智能编辑控制器自动选择该图元
                if hasattr(ctx.scene, 'view') and ctx.scene.view:
                    if hasattr(ctx.scene.view, 'smart_edit_controller'):
                        ctx.scene.view.smart_edit_controller.select_item(item_to_select, auto_select=True)
            
            print(f"[ArrowTool] 完成绘制")
