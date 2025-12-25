"""
椭圆工具
"""

from PyQt6.QtCore import QPointF, QRectF, Qt
from PyQt6.QtGui import QPen
from .base import Tool, ToolContext, color_with_opacity
from canvas.items import EllipseItem
from canvas.undo import AddItemCommand


class EllipseTool(Tool):
    """
    椭圆工具
    """
    
    id = "ellipse"
    
    # 最小绘制尺寸（像素），小于此值的绘制将被忽略
    MIN_SIZE = 10
    
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
            rect = QRectF(pos, pos)
            
            self.current_item = EllipseItem(rect, pen)
            ctx.scene.addItem(self.current_item)
            
            print(f"[EllipseTool] 开始绘制: {pos}")
    
    def on_move(self, pos: QPointF, ctx: ToolContext):
        if self.drawing and self.current_item:
            rect = QRectF(self.start_pos, pos).normalized()
            self.current_item.setRect(rect)
    
    def on_release(self, pos: QPointF, ctx: ToolContext):
        if self.drawing:
            self.drawing = False
            
            if self.current_item:
                # 检查绘制尺寸是否满足最小要求
                rect = QRectF(self.start_pos, pos).normalized()
                if rect.width() < self.MIN_SIZE or rect.height() < self.MIN_SIZE:
                    # 尺寸过小，取消绘制
                    ctx.scene.removeItem(self.current_item)
                    self.current_item = None
                    print(f"[EllipseTool] 绘制取消：尺寸过小 ({rect.width():.1f}x{rect.height():.1f} < {self.MIN_SIZE})")
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
            
            print(f"[EllipseTool] 完成绘制")
