"""
高亮笔工具
"""

from PySide6.QtCore import QPointF, Qt, QRectF
from PySide6.QtGui import QPen, QBrush
from .base import FreehandStroke, Tool, ToolContext, color_with_opacity, shift_held, shift_line_angle
from canvas.items import RectItem, StrokeItem
from canvas.undo import AddItemCommand
from .pen import commit_stroke


class HighlighterTool(Tool):
    """
    高亮笔工具 - 类似画笔但半透明
    画的过程中按住 Shift 从当前位置拉直线，角度吸附见 shift_line_angle
    """
    
    id = "highlighter"
    
    MODE_FREEHAND = "freehand"
    MODE_RECT = "rect"
    MIN_SIZE = 10

    def __init__(self):
        self.drawing = False
        self.current_item = None
        self.rect_item = None
        self.start_pos = None  # 矩形模式的起点
        self.stroke = None  # 自由模式的 FreehandStroke
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
            # 上一笔没收到松手（按着鼠标时焦点被抢走等），先照松手收下
            self._finish_freehand(ctx)
            self.draw_mode = self._load_draw_mode(ctx)
            if self.draw_mode == self.MODE_RECT:
                self._start_rect_draw(pos, ctx)
                return

            self.drawing = True
            self.stroke = FreehandStroke(pos, shift_line_angle(ctx), smooth=True)
            
            # 高亮笔颜色处理（使用当前透明度）
            base_opacity = ctx.opacity if ctx.opacity is not None else 0.5
            color = color_with_opacity(ctx.color, base_opacity)
            
            # 笔触更宽
            pen = QPen(color, ctx.stroke_width * 3)
            pen.setCapStyle(Qt.PenCapStyle.RoundCap)
            pen.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
            
            # 创建图元并添加到场景
            self.current_item = StrokeItem(self.stroke.path, pen, is_highlighter=True)
            ctx.scene.addItem(self.current_item)
    
    def on_move(self, pos: QPointF, ctx: ToolContext):
        if self.draw_mode == self.MODE_RECT:
            if self.drawing and self.rect_item and self.start_pos:
                rect = QRectF(self.start_pos, pos).normalized()
                self.rect_item.setRect(rect)
            return

        if self.drawing and self.current_item and self.stroke:
            if self.stroke.extend(pos, straight=shift_held()):
                self.current_item.extend_path(self.stroke.path, self.stroke.take_dirty_from())
    
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

        self._finish_freehand(ctx)

    def on_deactivate(self, ctx: ToolContext):
        # 画到一半切走工具，这一笔照常收下
        self._finish_freehand(ctx)
        super().on_deactivate(ctx)

    def _finish_freehand(self, ctx: ToolContext):
        item, stroke = self.current_item, self.stroke
        if item is None or stroke is None:
            return
        self.drawing = False
        self.current_item = None
        self.stroke = None
        commit_stroke(ctx, item, stroke)
 