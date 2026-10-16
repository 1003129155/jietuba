"""
画笔工具
"""

from PySide6.QtCore import QPointF, Qt
from PySide6.QtGui import QPen
from .base import FreehandStroke, Tool, ToolContext, color_with_opacity, shift_held, shift_line_angle
from canvas.items import StrokeItem
from canvas.undo import AddItemCommand


def commit_stroke(ctx: ToolContext, item: StrokeItem, stroke: FreehandStroke):
    """画完的笔迹交给撤销栈。它已经在场景里，推进撤销栈时 AddItemCommand 不会再加一次。"""
    if stroke.finish():
        item.extend_path(stroke.path, stroke.take_dirty_from())
    item.finish_path()
    ctx.undo_stack.push(AddItemCommand(ctx.scene, item))


class PenTool(Tool):
    """
    画笔工具 - 自由绘制
    画的过程中按住 Shift 从当前位置拉直线，角度吸附见 shift_line_angle
    """
    
    id = "pen"
    
    def __init__(self):
        self.drawing = False
        self.current_item = None
        self.stroke = None  # FreehandStroke
        self.line_style = "solid"  # 线条样式：solid（实线）或 dashed（虚线）
    
    def on_press(self, pos: QPointF, button, ctx: ToolContext):
        if button == Qt.MouseButton.LeftButton:
            # 上一笔没收到松手（按着鼠标时焦点被抢走等），先照松手收下
            self._finish(ctx)
            self.drawing = True
            
            # 从设置管理器获取线条样式
            if ctx.settings_manager:
                settings = ctx.settings_manager.get_tool_settings("pen")
                self.line_style = settings.get("line_style", "solid")
            
            self.stroke = FreehandStroke(pos, shift_line_angle(ctx), smooth=True)
            
            # 创建画笔
            pen_color = color_with_opacity(ctx.color, ctx.opacity)
            pen = QPen(pen_color, ctx.stroke_width)
            pen.setCapStyle(Qt.PenCapStyle.RoundCap)
            pen.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
            
            # 设置线条样式
            if self.line_style == "dashed":
                pen.setStyle(Qt.PenStyle.DashLine)
                # 自定义虚线模式：dash长度, 间隔长度
                pen.setDashPattern([3, 2])
            elif self.line_style == "dashed_dense":
                pen.setStyle(Qt.PenStyle.DashLine)
                # 更密集的虚线模式
                pen.setDashPattern([1, 2])
            else:
                pen.setStyle(Qt.PenStyle.SolidLine)
                pen.setDashPattern([])
            
            # 创建图元并添加到场景
            self.current_item = StrokeItem(self.stroke.path, pen, is_highlighter=False)
            ctx.scene.addItem(self.current_item)
    
    def on_move(self, pos: QPointF, ctx: ToolContext):
        if self.drawing and self.current_item and self.stroke:
            if self.stroke.extend(pos, straight=shift_held()):
                self.current_item.extend_path(self.stroke.path, self.stroke.take_dirty_from())
    
    def on_release(self, pos: QPointF, ctx: ToolContext):
        self._finish(ctx)

    def on_deactivate(self, ctx: ToolContext):
        # 画到一半切走工具，这一笔照常收下
        self._finish(ctx)
        super().on_deactivate(ctx)

    def _finish(self, ctx: ToolContext):
        item, stroke = self.current_item, self.stroke
        self.drawing = False
        self.current_item = None
        self.stroke = None
        if item is not None and stroke is not None:
            commit_stroke(ctx, item, stroke)
 