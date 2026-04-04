"""
高亮笔工具
"""

from PySide6.QtCore import QPointF, Qt, QRectF
from PySide6.QtGui import QPainterPath, QPen, QColor, QBrush
from .base import Tool, ToolContext, color_with_opacity
from canvas.items import StrokeItem, RectItem
from canvas.undo import AddItemCommand


class HighlighterTool(Tool):
    """
    高亮笔工具 - 类似画笔但半透明
    支持按住 Shift 键绘制水平或垂直直线
    """
    
    id = "highlighter"
    
    MODE_FREEHAND = "freehand"
    MODE_RECT = "rect"
    MIN_SIZE = 10

    def __init__(self):
        self.drawing = False
        self.current_item = None
        self.rect_item = None
        self.path = None
        self.start_pos = None  # 起始点
        self.line_lock_mode = None  # 直线锁定模式: 'horizontal' 或 'vertical' 或 None
        self.locked_coordinate = None  # 被锁定的坐标值
        self.draw_mode = self.MODE_FREEHAND

    def _load_draw_mode(self, ctx: ToolContext) -> str:
        if ctx.settings_manager:
            settings = ctx.settings_manager.get_tool_settings("highlighter")
            return settings.get("draw_mode", self.MODE_FREEHAND)
        return self.MODE_FREEHAND

    def _start_rect_draw(self, pos: QPointF, ctx: ToolContext):
        self.drawing = True
        self.start_pos = pos
        base_opacity = ctx.opacity if ctx.opacity is not None else 0.5
        color = color_with_opacity(ctx.color, base_opacity)

        pen = QPen(color, max(1.0, float(ctx.stroke_width)))
        pen.setCapStyle(Qt.PenCapStyle.RoundCap)
        pen.setJoinStyle(Qt.PenJoinStyle.RoundJoin)

        rect = RectItem(QRectF(pos, pos), pen)
        rect.setBrush(QBrush(color))
        rect.setZValue(10)
        rect.is_highlighter = True
        rect.is_highlighter_rect = True
        self.rect_item = rect
        ctx.scene.addItem(rect)
    
    def on_press(self, pos: QPointF, button, ctx: ToolContext):
        if button == Qt.MouseButton.LeftButton:
            self.draw_mode = self._load_draw_mode(ctx)
            if self.draw_mode == self.MODE_RECT:
                self._start_rect_draw(pos, ctx)
                return

            self.drawing = True
            self.start_pos = pos
            
            # 检测 Shift 键状态
            from PySide6.QtWidgets import QApplication
            modifiers = QApplication.keyboardModifiers()
            shift_pressed = bool(modifiers & Qt.KeyboardModifier.ShiftModifier)
            
            if shift_pressed:
                # Shift 按下：进入直线检测模式
                self.line_lock_mode = 'detecting'
                self.locked_coordinate = None
            else:
                # 正常自由绘制模式
                self.line_lock_mode = None
                self.locked_coordinate = None
            
            self.path = QPainterPath(pos)
            
            # 高亮笔颜色处理（使用当前透明度）
            base_opacity = ctx.opacity if ctx.opacity is not None else 0.5
            color = color_with_opacity(ctx.color, base_opacity)
            
            # 笔触更宽
            pen = QPen(color, ctx.stroke_width * 3)
            pen.setCapStyle(Qt.PenCapStyle.RoundCap)
            pen.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
            
            # 创建高亮图元
            self.current_item = StrokeItem(self.path, pen, is_highlighter=True)
            ctx.scene.addItem(self.current_item)
    
    def on_move(self, pos: QPointF, ctx: ToolContext):
        if self.draw_mode == self.MODE_RECT:
            if self.drawing and self.rect_item and self.start_pos:
                rect = QRectF(self.start_pos, pos).normalized()
                self.rect_item.setRect(rect)
            return

        if self.drawing and self.current_item and self.start_pos:
            paint_pos = QPointF(pos)
            
            # 处理直线绘制逻辑
            if self.line_lock_mode == 'detecting':
                # 首次移动时决定方向
                dx = abs(paint_pos.x() - self.start_pos.x())
                dy = abs(paint_pos.y() - self.start_pos.y())
                
                # 需要移动一定距离才能判断方向
                if dx + dy > 5:
                    if dx > dy:
                        # 水平移动更多，锁定 Y 坐标（水平线）
                        self.line_lock_mode = 'horizontal'
                        self.locked_coordinate = self.start_pos.y()
                        paint_pos.setY(self.locked_coordinate)
                    else:
                        # 垂直移动更多，锁定 X 坐标（垂直线）
                        self.line_lock_mode = 'vertical'
                        self.locked_coordinate = self.start_pos.x()
                        paint_pos.setX(self.locked_coordinate)
            elif self.line_lock_mode == 'horizontal':
                # 水平线模式，锁定 Y 坐标
                paint_pos.setY(self.locked_coordinate)
            elif self.line_lock_mode == 'vertical':
                # 垂直线模式，锁定 X 坐标
                paint_pos.setX(self.locked_coordinate)
            
            self.path.lineTo(paint_pos)
            self.current_item.setPath(self.path)
    
    def on_release(self, pos: QPointF, ctx: ToolContext):
        if self.draw_mode == self.MODE_RECT:
            if not self.drawing:
                return
            self.drawing = False
            if self.rect_item:
                rect = QRectF(self.start_pos, pos).normalized()
                if rect.width() < self.MIN_SIZE or rect.height() < self.MIN_SIZE:
                    ctx.scene.removeItem(self.rect_item)
                    self.rect_item = None
                    self.start_pos = None
                    return

                ctx.scene.removeItem(self.rect_item)
                command = AddItemCommand(ctx.scene, self.rect_item)
                ctx.undo_stack.push(command)

                item_to_select = self.rect_item
                self.rect_item = None
                self.start_pos = None

                ctx.scene.item_auto_select_requested.emit(item_to_select)
            return

        if self.drawing:
            self.drawing = False
            
            # 重置直线模式状态
            self.line_lock_mode = None
            self.locked_coordinate = None
            self.start_pos = None
            
            if self.current_item:
                ctx.scene.removeItem(self.current_item)
                command = AddItemCommand(ctx.scene, self.current_item)
                ctx.undo_stack.push(command)
                
                self.current_item = None
                self.path = None
 