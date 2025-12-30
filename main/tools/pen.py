"""
画笔工具
"""

from PyQt6.QtCore import QPointF, Qt
from PyQt6.QtGui import QPainterPath, QPen
from .base import Tool, ToolContext, color_with_opacity
from canvas.items import StrokeItem
from canvas.undo import AddItemCommand


class PenTool(Tool):
    """
    画笔工具 - 自由绘制
    支持按住 Shift 键绘制水平或垂直直线
    """
    
    id = "pen"
    
    def __init__(self):
        self.drawing = False
        self.current_item = None
        self.path = None
        self.start_pos = None  # 起始点
        self.line_lock_mode = None  # 直线锁定模式: 'horizontal' 或 'vertical' 或 None
        self.locked_coordinate = None  # 被锁定的坐标值
    
    def on_press(self, pos: QPointF, button, ctx: ToolContext):
        if button == Qt.MouseButton.LeftButton:
            self.drawing = True
            self.start_pos = pos
            
            # 检测 Shift 键状态
            from PyQt6.QtWidgets import QApplication
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
            
            # 创建路径
            self.path = QPainterPath(pos)
            
            # 创建画笔
            pen_color = color_with_opacity(ctx.color, ctx.opacity)
            pen = QPen(pen_color, ctx.stroke_width)
            pen.setCapStyle(Qt.PenCapStyle.RoundCap)
            pen.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
            
            # 创建图元并添加到场景
            self.current_item = StrokeItem(self.path, pen, is_highlighter=False)
            ctx.scene.addItem(self.current_item)
    
    def on_move(self, pos: QPointF, ctx: ToolContext):
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
            
            # 更新路径
            self.path.lineTo(paint_pos)
            self.current_item.setPath(self.path)
    
    def on_release(self, pos: QPointF, ctx: ToolContext):
        if self.drawing:
            self.drawing = False
            
            # 重置直线模式状态
            self.line_lock_mode = None
            self.locked_coordinate = None
            self.start_pos = None
            
            if self.current_item:
                # 提交到撤销栈
                # 注意：图元已经添加到场景了，AddItemCommand 需要处理这种情况
                # 通常 Command 的 redo 会执行 addItem。
                # 为了避免重复添加，我们可以先从场景移除，然后让 Command 去添加
                # 或者 Command 构造时知道 item 已经在场景中
                
                # 这里采用标准做法：先移除，交给 Command 管理
                ctx.scene.removeItem(self.current_item)
                
                command = AddItemCommand(ctx.scene, self.current_item)
                ctx.undo_stack.push(command)
                
                self.current_item = None
                self.path = None
