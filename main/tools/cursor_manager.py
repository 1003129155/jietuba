"""
光标管理器 - 统一管理工具鼠标样式和画笔大小指示器
"""

from PyQt6.QtCore import Qt, QPointF
from PyQt6.QtGui import QCursor, QPixmap, QPainter, QPen, QColor, QBrush
from PyQt6.QtWidgets import QGraphicsEllipseItem
from core import log_debug
from canvas.items import NumberItem


class CursorManager:
    """
    光标管理器
    
    功能：
    1. 为不同工具设置自定义光标（画笔/荧光笔用准星，序号用数字预览，其他用十字）
    2. 显示画笔大小圆圈指示器
    3. 管理光标状态切换
    """
    
    def __init__(self, view):
        """
        Args:
            view: CanvasView 实例
        """
        self.view = view
        self.scene = view.canvas_scene
        
        # 画笔大小指示器（圆圈）
        self.brush_indicator = None
        self.indicator_visible = False
        
        # 当前工具和画笔大小（用于缓存光标）
        self.current_tool_id = None
        self.current_brush_size = None
        self.current_cursor = None # 缓存当前光标对象
    
    def set_tool_cursor(self, tool_id: str, force: bool = False):
        """
        设置工具对应的光标
        - cursor: 箭头
        - pen/highlighter: 实心圆 + 准星
        - number: 圆圈 + 数字预览
        - 其他: 十字光标
        
        同时管理 SelectionItem 的交互状态
        
        Args:
            tool_id: 工具ID
            force: 是否强制更新
        """
        # 1. 管理 SelectionItem 的交互状态
        # 只有在 'cursor' 模式下，才允许 SelectionItem 响应鼠标悬停和点击
        if hasattr(self.scene, 'selection_item'):
            item = self.scene.selection_item
            is_cursor_mode = (tool_id == "cursor")
            
            # 设置是否接受悬停事件（控制光标变化）
            item.setAcceptHoverEvents(is_cursor_mode)
            
            # 设置是否接受鼠标点击（控制拖拽）
            item.setAcceptedMouseButtons(Qt.MouseButton.LeftButton if is_cursor_mode else Qt.MouseButton.NoButton)
            
            # 如果不是光标模式，强制重置 SelectionItem 的光标，防止残留
            if not is_cursor_mode:
                item.unsetCursor()

        # 2. 设置视图光标
        # 如果是光标工具，使用默认箭头
        if tool_id == "cursor":
            self.current_cursor = QCursor(Qt.CursorShape.ArrowCursor)
            self._apply_cursor(self.current_cursor)
            self.current_tool_id = tool_id
            return
        
        # 获取当前画笔大小
        brush_size = self.get_current_brush_size()
        
        # 工具切换时强制更新（即使笔刷大小相同）
        # 这样可以确保光标在任何情况下都正确显示
        tool_changed = (self.current_tool_id != tool_id)
        
        # 序号工具需要特殊处理：即使大小没变，数字也会变化，所以强制更新
        if tool_id == "number":
            force = True
        
        # 如果工具没变、大小也没变，且不强制更新，则可以跳过重新生成
        # 但仍然重新应用光标，防止被其他控件重置
        if not force and not tool_changed and self.current_brush_size == brush_size:
            # 即使没变，也重新应用一次，防止被 View 重置
            if self.current_cursor:
                self._apply_cursor(self.current_cursor)
            return
        
        # 创建自定义光标
        cursor = self.create_tool_cursor_with_size(tool_id, brush_size)
        if cursor is not None:
            self.current_cursor = cursor
            self.current_tool_id = tool_id
            self.current_brush_size = brush_size
            
            # 立即应用光标
            self._apply_cursor(cursor)
            
        else:
            # 如果加载失败，使用默认十字光标
            self.current_cursor = QCursor(Qt.CursorShape.CrossCursor)
            self._apply_cursor(self.current_cursor)
    
    def show_brush_indicator(self, pos: QPointF, size: int):
        """
        显示画笔大小指示器
        
        Args:
            pos: 鼠标位置（场景坐标）
            size: 画笔大小（像素）
        """
        # 移除旧的指示器
        self.hide_brush_indicator()
        
        # 创建新的圆圈
        radius = size / 2
        self.brush_indicator = QGraphicsEllipseItem(
            pos.x() - radius,
            pos.y() - radius,
            size,
            size
        )
        
        # 设置样式：虚线圆圈，半透明
        pen = QPen(QColor(100, 100, 100, 180))
        pen.setStyle(Qt.PenStyle.DashLine)
        pen.setWidth(1)
        self.brush_indicator.setPen(pen)
        
        # 不填充
        self.brush_indicator.setBrush(QBrush(Qt.BrushStyle.NoBrush))
        
        # 设置图层顺序（最顶层）
        self.brush_indicator.setZValue(10000)
        
        # 添加到场景
        self.scene.addItem(self.brush_indicator)
        self.indicator_visible = True
    
    def update_brush_indicator(self, pos: QPointF, size: int):
        """
        更新画笔指示器位置和大小
        
        Args:
            pos: 新位置（场景坐标）
            size: 画笔大小
        """
        if self.brush_indicator:
            # 更新位置和大小
            radius = size / 2
            self.brush_indicator.setRect(
                pos.x() - radius,
                pos.y() - radius,
                size,
                size
            )
        else:
            # 如果不存在则创建
            self.show_brush_indicator(pos, size)
    
    def hide_brush_indicator(self):
        """隐藏画笔大小指示器"""
        if self.brush_indicator:
            self.scene.removeItem(self.brush_indicator)
            self.brush_indicator = None
            self.indicator_visible = False
    
    def _create_crosshair_cursor(self, brush_size: int, color: QColor):
        """
        创建实心圆+准星样式的光标
        
        Args:
            brush_size: 画笔大小
            color: 当前颜色
        """
        # 准星线条长度
        line_len = 6
        # 准星与圆圈的间距
        gap = 4
        
        # 确保最小可见大小
        diameter = max(brush_size, 4)
        radius = diameter / 2
        
        # 计算画布大小：圆直径 + 两侧准星 + 留白
        # 考虑到描边宽度，适当增加画布尺寸
        total_size = int(diameter + 2 * (gap + line_len) + 8)
        center = total_size / 2
        
        pixmap = QPixmap(total_size, total_size)
        pixmap.fill(Qt.GlobalColor.transparent)
        
        painter = QPainter(pixmap)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        
        # 1. 绘制实心圆（画笔大小，填充当前颜色，白色描边）
        painter.setBrush(QBrush(color))
        # 白色描边，宽度1
        stroke_pen = QPen(QColor(255, 255, 255, 255))
        stroke_pen.setWidth(1)
        painter.setPen(stroke_pen)
        
        painter.drawEllipse(
            int(center - radius),
            int(center - radius),
            int(diameter),
            int(diameter)
        )
        
        # 2. 绘制准星（上下左右，当前颜色，加粗，带白色描边）
        # 定义四个方向的线段端点
        lines = [
            # 上
            (QPointF(center, center - radius - gap), QPointF(center, center - radius - gap - line_len)),
            # 下
            (QPointF(center, center + radius + gap), QPointF(center, center + radius + gap + line_len)),
            # 左
            (QPointF(center - radius - gap, center), QPointF(center - radius - gap - line_len, center)),
            # 右
            (QPointF(center + radius + gap, center), QPointF(center + radius + gap + line_len, center))
        ]
        
        # 先画白色描边（背景），加粗到 6px
        bg_pen = QPen(QColor(255, 255, 255, 255))
        bg_pen.setWidth(6)  # 增加宽度（原 4px → 6px）
        bg_pen.setCapStyle(Qt.PenCapStyle.RoundCap)
        painter.setPen(bg_pen)
        
        for p1, p2 in lines:
            painter.drawLine(p1, p2)
            
        # 再画颜色前景，加粗到 4px
        fg_pen = QPen(color)
        fg_pen.setWidth(4)  # 增加宽度（原 2px → 4px）
        fg_pen.setCapStyle(Qt.PenCapStyle.RoundCap)
        painter.setPen(fg_pen)
        
        for p1, p2 in lines:
            painter.drawLine(p1, p2)
        
        painter.end()
        
        return QCursor(pixmap, int(center), int(center))
    
    def _create_number_cursor(self, brush_size: int):
        """
        创建序号工具的光标（真实的圆圈+数字预览）
        
        Args:
            brush_size: 画笔大小（stroke_width）
        """
        # 获取当前工具上下文
        ctx = self.scene.tool_controller.context
        color = QColor(ctx.color) if ctx else QColor(Qt.GlobalColor.red)
        
        # 获取下一个序号数字（使用与绘制时相同的方法）
        from tools.number import NumberTool
        next_number = NumberTool.get_next_number(self.scene)
        
        log_debug(f"创建序号光标，数字={next_number}", "CursorManager")
        
        # 计算圆圈半径（与 NumberTool 中的计算一致）
        radius = NumberTool.get_radius_for_width(brush_size)
        diameter = radius * 2
        
        # 画布大小需要容纳圆圈 + 描边 + 留白
        stroke_width = brush_size
        total_size = int(diameter + stroke_width * 2 + 10)
        center = total_size / 2
        
        pixmap = QPixmap(total_size, total_size)
        pixmap.fill(Qt.GlobalColor.transparent)
        
        painter = QPainter(pixmap)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        
        # 1. 绘制圆圈（实心填充，与 NumberItem 保持一致）
        painter.setPen(Qt.PenStyle.NoPen)  # 无边框
        painter.setBrush(QBrush(color))    # 实心填充
        
        painter.drawEllipse(
            int(center - radius),
            int(center - radius),
            int(diameter),
            int(diameter)
        )
        
        # 2. 绘制数字（在圆圈中心）
        # 使用与 NumberItem 相同的字体大小逻辑
        font = painter.font()
        font_scale = getattr(NumberItem, "FONT_SCALE", 1.15)
        min_font_size = getattr(NumberItem, "MIN_FONT_SIZE", 10)
        font_size = max(min_font_size, int(radius * font_scale))
        font.setPixelSize(font_size)
        font.setBold(True)
        painter.setFont(font)
        
        # 设置文字颜色为白色（与实际绘制一致）
        painter.setPen(QPen(QColor(Qt.GlobalColor.white)))
        
        # 在圆圈中心绘制数字
        text_rect = pixmap.rect()
        painter.drawText(text_rect, Qt.AlignmentFlag.AlignCenter, str(next_number))
        
        painter.end()
        
        # 光标热点在中心
        hotspot_x = int(center)
        hotspot_y = int(center)
        return QCursor(pixmap, hotspot_x, hotspot_y)

    def create_tool_cursor_with_size(self, tool_id: str, brush_size: int):
        """
        创建工具光标
        
        Args:
            tool_id: 工具ID
            brush_size: 画笔大小（像素）
            
        Returns:
            QCursor 实例
        """
        # 针对画笔和荧光笔使用特殊的新样式（实心圆+准星）
        if tool_id in ["pen", "highlighter"]:
            # 获取当前颜色
            ctx = self.scene.tool_controller.context
            color = QColor(ctx.color) if ctx else QColor(Qt.GlobalColor.red)
            
            # 荧光笔实际绘制比准星粗很多（约3倍）
            real_size = brush_size
            if tool_id == "highlighter":
                real_size = brush_size * 3
                color.setAlpha(150) # 让光标也带点透明感，但比实际绘制(128)稍微实一点以便看清
            else:
                color.setAlpha(255) # 画笔通常是不透明的

            return self._create_crosshair_cursor(real_size, color)
        
        # 针对序号工具使用真实的序号预览（圆圈+数字）
        if tool_id == "number":
            return self._create_number_cursor(brush_size)

        # 其他工具统一使用十字光标（简化版）
        return QCursor(Qt.CursorShape.CrossCursor)
    
    def update_tool_cursor_size(self, brush_size: int):
        """
        更新当前工具光标的大小
        
        Args:
            brush_size: 新的画笔大小
        """
        if self.current_tool_id and self.current_tool_id != "cursor":
            self.current_brush_size = None  # 强制重新生成
            self.set_tool_cursor(self.current_tool_id)
    
    def update_tool_cursor_color(self, color: QColor):
        """
        更新当前工具光标的颜色
        
        Args:
            color: 新的颜色
        """
        # 画笔、荧光笔、序号工具的光标都跟颜色有关
        if self.current_tool_id in ["pen", "highlighter", "number"]:
            self.current_brush_size = None  # 强制重新生成
            self.set_tool_cursor(self.current_tool_id)
    
    def get_current_brush_size(self) -> int:
        """
        获取当前画笔大小
        
        Returns:
            画笔大小（像素）
        """
        ctx = self.scene.tool_controller.context
        return ctx.stroke_width if ctx else 2

    def _apply_cursor(self, cursor):
        """将光标同时应用到视图和 viewport，避免 Qt 将其还原为默认十字"""
        targets = [self.view]
        viewport = self.view.viewport() if hasattr(self.view, "viewport") else None
        if viewport and viewport is not self.view:
            targets.append(viewport)
        for widget in targets:
            widget.setCursor(cursor)
