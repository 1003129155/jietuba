"""
矩形工具
"""

from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import QPen
from .base import Tool, ToolContext, color_with_opacity
from canvas.items import RectItem
from canvas.undo import AddItemCommand
from core import log_debug


class RectTool(Tool):
    """
    矩形工具
    """
    
    id = "rect"
    
    # 最小绘制尺寸（像素），小于此值的绘制将被忽略
    MIN_SIZE = 10
    
    def __init__(self):
        self.drawing = False
        self.start_pos = None
        self.current_item = None
        self.line_style = "solid"
        self.corner_radius = 0.0
    
    def on_press(self, pos: QPointF, button, ctx: ToolContext):
        if button == Qt.MouseButton.LeftButton:
            self.drawing = True
            self.start_pos = pos
            
            pen_color = color_with_opacity(ctx.color, ctx.opacity)
            pen = QPen(pen_color, ctx.stroke_width)

            # 从设置管理器获取线条样式
            if ctx.settings_manager:
                settings = ctx.settings_manager.get_tool_settings("rect")
                self.line_style = settings.get("line_style", "solid")
                self.corner_radius = float(settings.get("corner_radius", 0))

            # 统一圆角端点/连接，避免虚线呈方块
            pen.setCapStyle(Qt.PenCapStyle.RoundCap)
            pen.setJoinStyle(Qt.PenJoinStyle.RoundJoin)

            if self.line_style == "dashed":
                pen.setStyle(Qt.PenStyle.DashLine)
                pen.setDashPattern([3, 2])
            elif self.line_style == "dashed_dense":
                pen.setStyle(Qt.PenStyle.DashLine)
                pen.setDashPattern([1, 2])
            else:
                pen.setStyle(Qt.PenStyle.SolidLine)
                pen.setDashPattern([])
            
            # 创建初始矩形 (大小为0)
            rect = QRectF(pos, pos)
            self.current_item = RectItem(rect, pen, self.corner_radius)
            ctx.scene.addItem(self.current_item)
            
            log_debug(f"开始绘制: {pos}", "RectTool")
    
    def on_move(self, pos: QPointF, ctx: ToolContext):
        if self.drawing and self.current_item:
            # 更新矩形
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
                    log_debug(f"绘制取消：尺寸过小 ({rect.width():.1f}x{rect.height():.1f} < {self.MIN_SIZE})", "RectTool")
                    return
                
                # 提交到撤销栈
                ctx.scene.removeItem(self.current_item)
                command = AddItemCommand(ctx.scene, self.current_item)
                ctx.undo_stack.push(command)
                
                # 绘制完成后自动选择（方便调整）
                item_to_select = self.current_item
                self.current_item = None
                
                # 通过信号通知自动选中
                ctx.scene.item_auto_select_requested.emit(item_to_select)
            
            log_debug("完成绘制", "RectTool")
 