"""
矢量绘图图元
定义画笔、形状、荧光笔等基于 QGraphicsItem 的图元
"""
from __future__ import annotations

import math
from PySide6.QtWidgets import QGraphicsPathItem, QGraphicsRectItem, QGraphicsEllipseItem, QGraphicsItem, QGraphicsTextItem, QGraphicsPixmapItem
from PySide6.QtGui import QPen, QPainter, QPainterPath, QColor, QFont, QPixmap, QPainterPathStroker, QBrush
from PySide6.QtCore import Qt, QRectF, QPointF
from core import log_warning, safe_event

class DrawingItemMixin:
    """绘图图元通用属性"""
    def _init_drawing_mixin(self):
        """PySide6 要求在 super().__init__() 之后显式调用，而非定义 __init__
        （防止协作式 MRO 链在 Qt C++ 初始化前调用 Qt 方法）"""
        self.setFlag(QGraphicsItem.GraphicsItemFlag.ItemIsSelectable, True)
        self.setFlag(QGraphicsItem.GraphicsItemFlag.ItemIsMovable, True)
        self.setAcceptHoverEvents(True)

    def _update_hover_cursor(self, event=None):
        if self.isSelected():
            self.setCursor(Qt.CursorShape.SizeAllCursor)
        else:
            self.unsetCursor()
        if event is not None:
            event.accept()

    def hoverEnterEvent(self, event):
        self._update_hover_cursor(event)

    def hoverMoveEvent(self, event):
        self._update_hover_cursor(event)

    def hoverLeaveEvent(self, event):
        self.unsetCursor()
        if event is not None:
            event.accept()

    # ====================================================================
    # 统一属性接口 — View 层通过这些方法修改图元，不直接操作内部属性
    # ====================================================================

    def set_stroke_width(self, width: float):
        """设置线宽/大小，子类应按自身语义重写"""
        pass  # 默认无操作

    def scale_stroke_width(self, scale: float) -> bool:
        """按比例缩放线宽/大小，返回是否处理成功"""
        return False  # 默认不处理

    def set_visual_opacity(self, opacity: float) -> bool:
        """设置视觉透明度（优先修改颜色 alpha，保持 item opacity=1.0），返回是否成功"""
        return False  # 默认不处理

    def get_stroke_width(self) -> float | None:
        """获取当前线宽/大小，返回 None 表示不适用"""
        return None

    def get_visual_opacity(self) -> float | None:
        """获取当前视觉透明度"""
        if hasattr(self, 'opacity'):
            return max(0.0, min(1.0, float(self.opacity())))
        return None

    def _can_show_hover(self) -> bool:
        """
        判断是否应该显示悬停光标
        通过 QGraphicsScene.views() 获取 view（Qt 内建方法，无循环引用问题）
        """
        scene = self.scene() if hasattr(self, 'scene') else None
        if not scene:
            return False
        views = scene.views()
        if not views:
            return False
        controller = getattr(views[0], "smart_edit_controller", None)
        if controller:
            try:
                return controller.can_show_hover_cursor(self)
            except Exception:
                return False
        return False

class StrokeItem(QGraphicsPathItem, DrawingItemMixin):
    """画笔/荧光笔图元"""
    
    def __init__(self, path: QPainterPath, pen: QPen, is_highlighter: bool = False):
        super().__init__(path)
        self._init_drawing_mixin()
        
        # 缓存 shape，避免移动时重复计算昂贵的 createStroke
        self._shape_cache = None
        
        self.setPen(pen)
        self.is_highlighter = is_highlighter
        
        if is_highlighter:
            # 荧光笔层级较低，但在背景之上
            self.setZValue(10)
        else:
            # 普通画笔层级较高
            self.setZValue(20)
            
    def setPen(self, pen: QPen):
        """重写 setPen 以清除 shape 缓存"""
        super().setPen(pen)
        self._shape_cache = None
        
    def setPath(self, path: QPainterPath):
        """重写 setPath 以清除 shape 缓存"""
        super().setPath(path)
        self._shape_cache = None
            
    def shape(self):
        """
        重写 shape 以增加点击容错范围
        使用 QPainterPathStroker 生成比视觉路径更宽的点击区域
        """
        # 如果有缓存，直接返回
        if self._shape_cache is not None:
            return self._shape_cache
            
        path = self.path()
        if path.isEmpty():
            return path
            
        # 创建路径描边器
        stroker = QPainterPathStroker()
        # 设置宽度：当前笔触宽度 + 额外旷量(20px)
        # 这样即使是细线，也有至少 20px 的点击范围
        # 同时也方便移动，因为点击范围变大了
        stroker.setWidth(self.pen().widthF() + 20)
        stroker.setCapStyle(Qt.PenCapStyle.RoundCap)
        stroker.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
        
        # 生成扩大的形状路径并缓存
        self._shape_cache = stroker.createStroke(path)
        return self._shape_cache

    def paint(self, painter, option, widget=None):
        try:
            if self.is_highlighter:
                # 荧光笔使用正片叠底
                painter.setCompositionMode(QPainter.CompositionMode.CompositionMode_Multiply)
            
            # 优化渲染质量
            painter.setRenderHint(QPainter.RenderHint.Antialiasing)
            super().paint(painter, option, widget)
        except KeyboardInterrupt:
            # 传递键盘中断，允许用户终止程序
            raise
        except Exception as e:
            # 绘制错误记录日志，避免崩溃
            log_warning(f"DrawingItem paint 异常: {e}", "Canvas")

    # -- 统一属性接口 --

    def set_stroke_width(self, width: float):
        pen = self.pen()
        pen.setWidthF(max(1.0, float(width)))
        self.setPen(pen)
        self.update()

    def scale_stroke_width(self, scale: float) -> bool:
        pen = self.pen()
        pen.setWidthF(max(1.0, pen.widthF() * scale))
        self.setPen(pen)
        self.update()
        return True

    def set_visual_opacity(self, opacity: float) -> bool:
        opacity = max(0.0, min(1.0, float(opacity)))
        pen = QPen(self.pen())
        color = QColor(pen.color())
        color.setAlphaF(opacity)
        pen.setColor(color)
        self.setPen(pen)
        self.setOpacity(1.0)
        self.update()
        return True

    def get_stroke_width(self) -> float | None:
        width = float(self.pen().widthF())
        if getattr(self, 'is_highlighter', False):
            width = width / 3.0
        return width

    def get_visual_opacity(self) -> float | None:
        direct = max(0.0, min(1.0, float(self.opacity())))
        if direct < 0.999:
            return direct
        return self.pen().color().alphaF()

class ShapeItemMixin(DrawingItemMixin):
    """形状图元通用逻辑"""
    def __init__(self, pen: QPen):
        # 注意：不调用 _init_drawing_mixin()，形状图元直接在自身 __init__ 中设置 flags
        self.setPen(pen)
        self.setZValue(20)

class RectItem(QGraphicsRectItem):
    """矩形图元"""
    CLICK_MARGIN = 4  # 点击旷量（像素/每侧）
    def __init__(self, rect: QRectF, pen: QPen, corner_radius: float = 0.0):
        # 使用 QRectF 参数初始化
        super().__init__(rect)
        # 设置样式和属性
        self.setPen(pen)
        self.setBrush(QBrush(Qt.BrushStyle.NoBrush))
        self.setZValue(20)
        self._shape_cache = None
        self._hovered = False
        self._corner_radius = max(0.0, float(corner_radius))
        # 设置可选择和可移动
        self.setFlag(QGraphicsItem.GraphicsItemFlag.ItemIsSelectable, True)
        self.setFlag(QGraphicsItem.GraphicsItemFlag.ItemIsMovable, True)
        self.setAcceptHoverEvents(True)

    def setPen(self, pen: QPen):
        super().setPen(pen)
        self._shape_cache = None

    def setRect(self, rect: QRectF):
        super().setRect(rect)
        self._shape_cache = None

    def get_corner_radius(self) -> float:
        return getattr(self, '_corner_radius', 0.0)

    def set_corner_radius(self, radius: float):
        self._corner_radius = max(0.0, float(radius))
        self._shape_cache = None
        self.update()

    def shape(self):
        # 仅使用描边路径，避免点击到内部空白区域
        if self._shape_cache is not None:
            return self._shape_cache

        path = QPainterPath()
        r = self.get_corner_radius()
        if r > 0:
            path.addRoundedRect(self.rect(), r, r)
        else:
            path.addRect(self.rect())

        # 荧光笔矩形为实心，允许点击内部区域
        if getattr(self, "is_highlighter_rect", False):
            self._shape_cache = path
            return self._shape_cache

        stroker = QPainterPathStroker()
        stroker.setWidth(self.pen().widthF() + self.CLICK_MARGIN * 2)
        stroker.setCapStyle(Qt.PenCapStyle.SquareCap)
        stroker.setJoinStyle(Qt.PenJoinStyle.MiterJoin)

        self._shape_cache = stroker.createStroke(path)
        return self._shape_cache

    def boundingRect(self):
        rect = super().boundingRect()
        extra = max(0.0, self.pen().widthF() / 2.0) + self.CLICK_MARGIN
        return rect.adjusted(-extra, -extra, extra, extra)

    def _can_show_hover(self) -> bool:
        """判断是否应该显示悬停光标"""
        scene = self.scene() if hasattr(self, 'scene') else None
        if not scene:
            return False
        views = scene.views()
        if not views:
            return False
        controller = getattr(views[0], "smart_edit_controller", None)
        if controller:
            try:
                return controller.can_show_hover_cursor(self)
            except Exception:
                return False
        return False

    def paint(self, painter, option, widget=None):
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)

        if getattr(self, "is_highlighter", False):
            painter.setCompositionMode(QPainter.CompositionMode.CompositionMode_Multiply)

        pen = QPen(self.pen())
        if pen.style() in (Qt.PenStyle.DashLine, Qt.PenStyle.CustomDashLine):
            pen.setCapStyle(Qt.PenCapStyle.RoundCap)
            pen.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
        painter.setPen(pen)
        painter.setBrush(self.brush())
        r = self.get_corner_radius()
        if r > 0:
            painter.drawRoundedRect(self.rect(), r, r)
        else:
            painter.drawRect(self.rect())

        if (self.isSelected() or self._hovered) and self._can_show_hover():
            selection_pen = QPen(QColor(0, 180, 255, 230), 2, Qt.PenStyle.DashLine)
            selection_pen.setCosmetic(True)
            painter.setBrush(Qt.BrushStyle.NoBrush)
            painter.setPen(selection_pen)
            if r > 0:
                painter.drawRoundedRect(self.rect(), r, r)
            else:
                painter.drawRect(self.rect())

    def hoverEnterEvent(self, event):
        if not self._can_show_hover():
            self._hovered = False
            self.update()
            self.unsetCursor()
            event.accept()
            return
        self._hovered = True
        self.update()
        self.setCursor(Qt.CursorShape.SizeAllCursor)
        event.accept()

    def hoverMoveEvent(self, event):
        if not self._can_show_hover():
            self.unsetCursor()
            event.accept()
            return
        self.setCursor(Qt.CursorShape.SizeAllCursor)
        event.accept()

    def hoverLeaveEvent(self, event):
        self._hovered = False
        self.update()
        self.unsetCursor()
        event.accept()

    # -- 统一属性接口 --

    def set_stroke_width(self, width: float):
        pen = self.pen()
        pen.setWidthF(max(1.0, float(width)))
        self.setPen(pen)
        self.update()

    def scale_stroke_width(self, scale: float) -> bool:
        pen = self.pen()
        pen.setWidthF(max(1.0, pen.widthF() * scale))
        self.setPen(pen)
        self.update()
        return True

    def set_visual_opacity(self, opacity: float) -> bool:
        opacity = max(0.0, min(1.0, float(opacity)))
        pen = QPen(self.pen())
        color = QColor(pen.color())
        color.setAlphaF(opacity)
        pen.setColor(color)
        self.setPen(pen)
        # 荧光笔矩形同时更新 brush alpha
        if getattr(self, "is_highlighter_rect", False):
            brush = QBrush(self.brush())
            brush_color = QColor(brush.color())
            brush_color.setAlphaF(opacity)
            brush.setColor(brush_color)
            self.setBrush(brush)
        self.setOpacity(1.0)
        self.update()
        return True

    def get_stroke_width(self) -> float | None:
        return float(self.pen().widthF())

    def get_visual_opacity(self) -> float | None:
        direct = max(0.0, min(1.0, float(self.opacity())))
        if direct < 0.999:
            return direct
        return self.pen().color().alphaF()

class EllipseItem(QGraphicsEllipseItem):
    """椭圆图元"""
    CLICK_MARGIN = 4  # 点击旷量（像素/每侧）
    def __init__(self, rect: QRectF, pen: QPen):
        # 使用 QRectF 参数初始化
        super().__init__(rect)
        # 设置样式和属性
        self.setPen(pen)
        self.setBrush(QBrush(Qt.BrushStyle.NoBrush))
        self.setZValue(20)
        self._shape_cache = None
        self._hovered = False
        # 设置可选择和可移动
        self.setFlag(QGraphicsItem.GraphicsItemFlag.ItemIsSelectable, True)
        self.setFlag(QGraphicsItem.GraphicsItemFlag.ItemIsMovable, True)
        self.setAcceptHoverEvents(True)

    def setPen(self, pen: QPen):
        super().setPen(pen)
        self._shape_cache = None

    def setRect(self, rect: QRectF):
        super().setRect(rect)
        self._shape_cache = None

    def shape(self):
        # 仅使用描边路径，避免点击到内部空白区域
        if self._shape_cache is not None:
            return self._shape_cache

        path = QPainterPath()
        path.addEllipse(self.rect())

        stroker = QPainterPathStroker()
        stroker.setWidth(self.pen().widthF() + self.CLICK_MARGIN * 2)
        stroker.setCapStyle(Qt.PenCapStyle.RoundCap)
        stroker.setJoinStyle(Qt.PenJoinStyle.RoundJoin)

        self._shape_cache = stroker.createStroke(path)
        return self._shape_cache

    def boundingRect(self):
        rect = super().boundingRect()
        extra = max(0.0, self.pen().widthF() / 2.0) + self.CLICK_MARGIN
        return rect.adjusted(-extra, -extra, extra, extra)

    def _can_show_hover(self) -> bool:
        """判断是否应该显示悬停光标"""
        scene = self.scene() if hasattr(self, 'scene') else None
        if not scene:
            return False
        views = scene.views()
        if not views:
            return False
        controller = getattr(views[0], "smart_edit_controller", None)
        if controller:
            try:
                return controller.can_show_hover_cursor(self)
            except Exception:
                return False
        return False

    def paint(self, painter, option, widget=None):
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)

        # 绘制椭圆本体（避免默认选中矩形框）
        painter.setPen(self.pen())
        painter.setBrush(self.brush())
        painter.drawEllipse(self.rect())

        # 选中或悬停时绘制椭圆虚线轮廓
        if (self.isSelected() or self._hovered) and self._can_show_hover():
            selection_pen = QPen(QColor(0, 180, 255, 230), 2, Qt.PenStyle.DashLine)
            selection_pen.setCosmetic(True)
            painter.setBrush(Qt.BrushStyle.NoBrush)
            painter.setPen(selection_pen)
            painter.drawEllipse(self.rect())

    def hoverEnterEvent(self, event):
        if not self._can_show_hover():
            self._hovered = False
            self.update()
            self.unsetCursor()
            event.accept()
            return
        self._hovered = True
        self.update()
        self.setCursor(Qt.CursorShape.SizeAllCursor)
        event.accept()

    def hoverMoveEvent(self, event):
        if not self._can_show_hover():
            self.unsetCursor()
            event.accept()
            return
        self.setCursor(Qt.CursorShape.SizeAllCursor)
        event.accept()

    def hoverLeaveEvent(self, event):
        self._hovered = False
        self.update()
        self.unsetCursor()
        event.accept()

    # -- 统一属性接口 --

    def set_stroke_width(self, width: float):
        pen = self.pen()
        pen.setWidthF(max(1.0, float(width)))
        self.setPen(pen)
        self.update()

    def scale_stroke_width(self, scale: float) -> bool:
        pen = self.pen()
        pen.setWidthF(max(1.0, pen.widthF() * scale))
        self.setPen(pen)
        self.update()
        return True

    def set_visual_opacity(self, opacity: float) -> bool:
        opacity = max(0.0, min(1.0, float(opacity)))
        pen = QPen(self.pen())
        color = QColor(pen.color())
        color.setAlphaF(opacity)
        pen.setColor(color)
        self.setPen(pen)
        self.setOpacity(1.0)
        self.update()
        return True

    def get_stroke_width(self) -> float | None:
        return float(self.pen().widthF())

    def get_visual_opacity(self) -> float | None:
        direct = max(0.0, min(1.0, float(self.opacity())))
        if direct < 0.999:
            return direct
        return self.pen().color().alphaF()


class ArrowItem(QGraphicsPathItem, DrawingItemMixin):
    """
    箭头图元 - 平滑箭头，支持弯曲
    
    始终是3点结构（start / control / end）：
    - control 未修改时 → 自动保持在中点，表现为直线箭头
    - control 被拖动后 → 变成弯曲箭头
    - 撤销可以恢复到直线状态
    
    箭头样式：
    - "single": 单头箭头（默认，只有终点有箭头）
    - "double": 双头箭头（起点和终点都有箭头）
    - "bar": 工字箭头（两端为短横杠）
    """
    
    # 箭头样式常量
    STYLE_SINGLE = "single"
    STYLE_DOUBLE = "double"
    STYLE_BAR = "bar"
    CLICK_MARGIN = 4  # 点击旷量（像素/每侧）
    
    def __init__(self, start_pos: QPointF, end_pos: QPointF, pen: QPen, arrow_style: str = "single"):
        super().__init__()
        self._init_drawing_mixin()
        self.setPen(QPen(Qt.PenStyle.NoPen))  # 不使用轮廓线
        self.setBrush(pen.color())  # 使用填充
        self.setZValue(20)
        self._hovered = False
        
        self.start_pos = start_pos
        self.end_pos = end_pos
        # 控制点初始化为中点
        self._control_pos = QPointF(
            (start_pos.x() + end_pos.x()) / 2,
            (start_pos.y() + end_pos.y()) / 2
        )
        # 标记控制点是否被用户修改过（决定是直线还是曲线）
        self._control_modified = False
        
        self.base_width = pen.width()
        self.color = pen.color()
        self._shape_cache = None
        # 箭头样式：single（单头）或 double（双头）
        self._arrow_style = arrow_style if arrow_style in (self.STYLE_SINGLE, self.STYLE_DOUBLE, self.STYLE_BAR) else self.STYLE_SINGLE
        self.update_geometry()

    def setPath(self, path: QPainterPath):
        super().setPath(path)
        self._shape_cache = None

    def shape(self):
        if self._shape_cache is not None:
            return self._shape_cache

        path = self.path()
        if path.isEmpty():
            return path

        stroker = QPainterPathStroker()
        stroker.setWidth(max(1.0, float(self.base_width)) + self.CLICK_MARGIN * 2)
        stroker.setCapStyle(Qt.PenCapStyle.RoundCap)
        stroker.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
        self._shape_cache = stroker.createStroke(path)
        return self._shape_cache

    @property
    def arrow_style(self) -> str:
        """获取箭头样式"""
        return self._arrow_style
    
    @arrow_style.setter
    def arrow_style(self, value: str):
        """设置箭头样式"""
        if value in (self.STYLE_SINGLE, self.STYLE_DOUBLE, self.STYLE_BAR):
            self._arrow_style = value
            self.update_geometry()
    
    @property
    def control_pos(self) -> QPointF:
        """获取控制点位置"""
        if self._control_modified:
            return self._control_pos
        else:
            # 未修改时，返回当前的中点（跟随 start/end）
            return QPointF(
                (self.start_pos.x() + self.end_pos.x()) / 2,
                (self.start_pos.y() + self.end_pos.y()) / 2
            )
    
    @control_pos.setter
    def control_pos(self, value):
        """设置控制点（用于撤销恢复等）"""
        if value is None:
            self._control_modified = False
            self._control_pos = QPointF(
                (self.start_pos.x() + self.end_pos.x()) / 2,
                (self.start_pos.y() + self.end_pos.y()) / 2
            )
        else:
            self._control_pos = QPointF(value)
            # 注意：这里不自动设置 _control_modified
            # 因为撤销恢复时可能恢复到中点位置但仍是"未修改"状态
        
    def set_positions(self, start_pos: QPointF, end_pos: QPointF):
        """设置起点和终点"""
        self.start_pos = start_pos
        self.end_pos = end_pos
        
        if not self._control_modified:
            # 控制点未修改时，自动更新到新的中点
            self._control_pos = QPointF(
                (start_pos.x() + end_pos.x()) / 2,
                (start_pos.y() + end_pos.y()) / 2
            )
        # 如果控制点已修改，保持其绝对位置不变
        
        self.update_geometry()
        
    def set_control_point(self, control_pos: QPointF):
        """用户拖动控制点时调用 - 标记为已修改"""
        self._control_pos = QPointF(control_pos)
        self._control_modified = True
        self.update_geometry()
        
    def reset_control_point(self):
        """重置控制点（恢复直线箭头）"""
        self._control_modified = False
        self._control_pos = QPointF(
            (self.start_pos.x() + self.end_pos.x()) / 2,
            (self.start_pos.y() + self.end_pos.y()) / 2
        )
        self.update_geometry()
        
    def get_control_point(self) -> QPointF:
        """获取控制点位置（始终返回有效位置）"""
        return self.control_pos
    
    def is_curved(self) -> bool:
        """是否是弯曲箭头"""
        return self._control_modified
        
    def update_geometry(self):
        """更新箭头几何形状"""
        if self._control_modified:
            self._update_curved_geometry()
        else:
            self._update_straight_geometry()
    
    def _update_straight_geometry(self):
        """直线箭头几何（支持单头和双头）"""
        import math
        
        dx = self.end_pos.x() - self.start_pos.x()
        dy = self.end_pos.y() - self.start_pos.y()
        length = math.sqrt(dx * dx + dy * dy)
        
        if length < 0.1:
            return
        
        # 单位向量和垂直向量
        unit_x = dx / length
        unit_y = dy / length
        perp_x = -unit_y
        perp_y = unit_x
        
        # 参数设计
        base_width = self.base_width
        
        # 箭头三角形参数
        arrow_head_length = min(length * 0.25, max(20, base_width * 4.5))
        arrow_head_width = max(base_width * 1.8, 7)
        
        # 双头/工字样式
        is_double = self._arrow_style == self.STYLE_DOUBLE
        is_bar = self._arrow_style == self.STYLE_BAR
        # 颈部宽度（双头时用“中间值”统一粗细）
        base_shaft_width = base_width * 0.9
        neck_width = (arrow_head_width * 0.98 + base_shaft_width) / 2 if is_double else arrow_head_width * 0.85
        
        # 箭杆结束点（终点箭头颈部位置）
        neck_end_x = self.end_pos.x() - arrow_head_length * unit_x
        neck_end_y = self.end_pos.y() - arrow_head_length * unit_y
        
        if is_double:
            neck_start_x = self.start_pos.x() + arrow_head_length * unit_x
            neck_start_y = self.start_pos.y() + arrow_head_length * unit_y
        
        # 箭杆宽度
        shaft_width = base_shaft_width
        
        # 构建完整路径
        path = QPainterPath()
        
        if is_bar:
            # === 工字箭头 ===
            # 三条横杠粗细一致，都使用相同的宽度
            bar_thickness = max(base_width * 1.8, 6)  # 横杠厚度（统一）
            # cap_width 控制“胳膊”左右长度，bar_thickness 控制上下厚度
            cap_width = max(bar_thickness * 2.2, base_width * 3.5)
            shaft_width = bar_thickness  # 中间横杠宽度
            cap_length = min(arrow_head_length * 0.5, max(base_width * 2.0, 8))  # 横杠长度

            start_cap_inner_x = self.start_pos.x() + unit_x * cap_length
            start_cap_inner_y = self.start_pos.y() + unit_y * cap_length
            end_cap_inner_x = self.end_pos.x() - unit_x * cap_length
            end_cap_inner_y = self.end_pos.y() - unit_y * cap_length

            start_cap_left = QPointF(self.start_pos.x() + perp_x * cap_width,
                                     self.start_pos.y() + perp_y * cap_width)
            start_cap_right = QPointF(self.start_pos.x() - perp_x * cap_width,
                                      self.start_pos.y() - perp_y * cap_width)
            start_cap_inner_left = QPointF(start_cap_inner_x + perp_x * cap_width,
                                           start_cap_inner_y + perp_y * cap_width)
            start_cap_inner_right = QPointF(start_cap_inner_x - perp_x * cap_width,
                                            start_cap_inner_y - perp_y * cap_width)
            start_inner_left = QPointF(start_cap_inner_x + perp_x * shaft_width / 2,
                                       start_cap_inner_y + perp_y * shaft_width / 2)
            start_inner_right = QPointF(start_cap_inner_x - perp_x * shaft_width / 2,
                                        start_cap_inner_y - perp_y * shaft_width / 2)

            end_inner_left = QPointF(end_cap_inner_x + perp_x * shaft_width / 2,
                                     end_cap_inner_y + perp_y * shaft_width / 2)
            end_inner_right = QPointF(end_cap_inner_x - perp_x * shaft_width / 2,
                                      end_cap_inner_y - perp_y * shaft_width / 2)
            end_cap_inner_left = QPointF(end_cap_inner_x + perp_x * cap_width,
                                         end_cap_inner_y + perp_y * cap_width)
            end_cap_inner_right = QPointF(end_cap_inner_x - perp_x * cap_width,
                                          end_cap_inner_y - perp_y * cap_width)
            end_cap_left = QPointF(self.end_pos.x() + perp_x * cap_width,
                                   self.end_pos.y() + perp_y * cap_width)
            end_cap_right = QPointF(self.end_pos.x() - perp_x * cap_width,
                                    self.end_pos.y() - perp_y * cap_width)

            path.moveTo(start_cap_left)
            path.lineTo(start_cap_right)
            path.lineTo(start_cap_inner_right)
            path.lineTo(start_inner_right)
            path.lineTo(end_inner_right)
            path.lineTo(end_cap_inner_right)
            path.lineTo(end_cap_right)
            path.lineTo(end_cap_left)
            path.lineTo(end_cap_inner_left)
            path.lineTo(end_inner_left)
            path.lineTo(start_inner_left)
            path.lineTo(start_cap_inner_left)

        elif is_double:
            # === 双头箭头 ===
            # 起点箭头的左翼开始
            start_wing_left_x = neck_start_x + perp_x * arrow_head_width
            start_wing_left_y = neck_start_y + perp_y * arrow_head_width
            
            path.moveTo(start_wing_left_x, start_wing_left_y)
            
            # 起点尖端
            path.lineTo(self.start_pos.x(), self.start_pos.y())
            
            # 起点箭头的右翼
            start_wing_right_x = neck_start_x - perp_x * arrow_head_width
            start_wing_right_y = neck_start_y - perp_y * arrow_head_width
            path.lineTo(start_wing_right_x, start_wing_right_y)
            
            # 起点箭头凹陷效果
            # 双头箭头的凹陷太深会出现“缺一块”的视觉断口，适当减小
            start_notch_depth = arrow_head_length * 0.05
            start_notch_x = neck_start_x + unit_x * start_notch_depth
            start_notch_y = neck_start_y + unit_y * start_notch_depth
            
            path.quadTo(QPointF(start_notch_x, start_notch_y),
                       QPointF(neck_start_x - perp_x * neck_width / 2,
                              neck_start_y - perp_y * neck_width / 2))
            
            # 箭杆下半部分（从起点颈部到终点颈部）
            path.lineTo(neck_end_x - perp_x * neck_width / 2,
                       neck_end_y - perp_y * neck_width / 2)
            
            # 终点箭头右翼
            wing_right_x = neck_end_x - perp_x * arrow_head_width
            wing_right_y = neck_end_y - perp_y * arrow_head_width
            path.lineTo(wing_right_x, wing_right_y)
            
            # 终点尖端
            path.lineTo(self.end_pos.x(), self.end_pos.y())
            
            # 终点箭头左翼
            wing_left_x = neck_end_x + perp_x * arrow_head_width
            wing_left_y = neck_end_y + perp_y * arrow_head_width
            path.lineTo(wing_left_x, wing_left_y)
            
            # 终点箭头凹陷效果
            # 双头箭头的凹陷太深会出现“缺一块”的视觉断口，适当减小
            notch_depth = arrow_head_length * 0.05
            notch_x = neck_end_x - unit_x * notch_depth
            notch_y = neck_end_y - unit_y * notch_depth
            
            path.quadTo(QPointF(notch_x, notch_y),
                       QPointF(neck_end_x + perp_x * neck_width / 2,
                              neck_end_y + perp_y * neck_width / 2))
            
            # 箭杆上半部分（从终点颈部回到起点颈部）
            path.lineTo(neck_start_x + perp_x * neck_width / 2,
                       neck_start_y + perp_y * neck_width / 2)
            
        else:
            # === 单头箭头（原版算法） ===
            # 尾巴起点宽度（尖细）
            tail_width = base_width * 0.15
            
            # 箭杆中段宽度（最粗的部分）
            mid_point = 0.7
            mid_x = self.start_pos.x() + dx * mid_point
            mid_y = self.start_pos.y() + dy * mid_point
            mid_width = base_width * 0.9
            
            # === 箭杆部分 ===
            # 上半部分
            path.moveTo(self.start_pos.x() + perp_x * tail_width / 2,
                       self.start_pos.y() + perp_y * tail_width / 2)
            
            path.lineTo(mid_x + perp_x * mid_width / 2,
                       mid_y + perp_y * mid_width / 2)
            
            path.lineTo(neck_end_x + perp_x * neck_width / 2,
                       neck_end_y + perp_y * neck_width / 2)
            
            # === 箭头三角形部分（带凹陷） ===
            # 左翼
            wing_left_x = neck_end_x + perp_x * arrow_head_width
            wing_left_y = neck_end_y + perp_y * arrow_head_width
            
            path.lineTo(wing_left_x, wing_left_y)
            
            # 箭头尖端
            path.lineTo(self.end_pos.x(), self.end_pos.y())
            
            # 右翼
            wing_right_x = neck_end_x - perp_x * arrow_head_width
            wing_right_y = neck_end_y - perp_y * arrow_head_width
            
            path.lineTo(wing_right_x, wing_right_y)
            
            # 后弯曲效果（贝塞尔曲线）
            # 限制凹陷深度,避免短箭头自交导致白洞
            notch_depth = min(
                arrow_head_length * 0.05,  # 降低凹陷比例,与双头箭头保持一致
                neck_width * 0.4           # 最大不超过颈部宽度的 40%
            )
            notch_x = neck_end_x - unit_x * notch_depth
            notch_y = neck_end_y - unit_y * notch_depth
            
            path.quadTo(QPointF(notch_x, notch_y),
                       QPointF(neck_end_x - perp_x * neck_width / 2,
                              neck_end_y - perp_y * neck_width / 2))
            
            # === 箭杆下半部分（镜像） ===
            path.lineTo(mid_x - perp_x * mid_width / 2,
                       mid_y - perp_y * mid_width / 2)
            
            path.lineTo(self.start_pos.x() - perp_x * tail_width / 2,
                       self.start_pos.y() - perp_y * tail_width / 2)
        
        path.closeSubpath()
        
        self.setPath(path)
    
    def _update_curved_geometry(self):
        """弯曲箭头几何 - 中间点在曲线上（三点定曲线），支持单头和双头"""
        import math
        
        # 中间点 M 是用户拖拽的点，它应该在曲线上
        # 对于二次贝塞尔曲线 B(t) = (1-t)²P0 + 2(1-t)tP1 + t²P2
        # 我们要让 B(0.5) = M，需要计算真正的控制点 P1
        # M = 0.25*P0 + 0.5*P1 + 0.25*P2
        # P1 = 2*M - 0.5*P0 - 0.5*P2
        
        mid_point = self.control_pos  # 用户指定的中间点（在曲线上）
        
        # 计算真正的贝塞尔控制点
        bezier_control = QPointF(
            2 * mid_point.x() - 0.5 * self.start_pos.x() - 0.5 * self.end_pos.x(),
            2 * mid_point.y() - 0.5 * self.start_pos.y() - 0.5 * self.end_pos.y()
        )
        
        # 计算曲线末端的切线方向（用于箭头朝向）
        # B'(1) = 2(P2-P1) 即终点处切线方向为 bezier_control -> end
        dx_end = self.end_pos.x() - bezier_control.x()
        dy_end = self.end_pos.y() - bezier_control.y()
        length_end = math.sqrt(dx_end * dx_end + dy_end * dy_end)
        
        if length_end < 0.1:
            self._update_straight_geometry()
            return
        
        # 起点处的切线方向 B'(0) = 2(P1-P0)
        dx_start = bezier_control.x() - self.start_pos.x()
        dy_start = bezier_control.y() - self.start_pos.y()
        length_start = math.sqrt(dx_start * dx_start + dy_start * dy_start)
        
        if length_start < 0.1:
            self._update_straight_geometry()
            return
        
        # 终点处的单位向量和垂直向量
        unit_x_end = dx_end / length_end
        unit_y_end = dy_end / length_end
        perp_x_end = -unit_y_end
        perp_y_end = unit_x_end
        
        # 起点处的单位向量和垂直向量
        unit_x_start = dx_start / length_start
        unit_y_start = dy_start / length_start
        perp_x_start = -unit_y_start
        perp_y_start = unit_x_start
        
        # 参数设计
        base_width = self.base_width
        
        # 估算总曲线长度（简单近似）
        total_length = length_start + length_end
        
        # 箭头三角形参数
        arrow_head_length = min(total_length * 0.15, max(20, base_width * 4.5))
        arrow_head_width = max(base_width * 1.8, 7)
        
        # 双头/工字样式
        is_double = self._arrow_style == self.STYLE_DOUBLE
        is_bar = self._arrow_style == self.STYLE_BAR
        # 颈部宽度（双头时用“中间值”统一粗细）
        base_shaft_width = base_width * 0.9
        neck_width = (arrow_head_width * 0.98 + base_shaft_width) / 2 if is_double else arrow_head_width * 0.85
        
        # 箭杆结束点（箭头颈部位置）- 沿终点切线方向回退
        neck_end_x = self.end_pos.x() - arrow_head_length * unit_x_end
        neck_end_y = self.end_pos.y() - arrow_head_length * unit_y_end
        
        if is_double:
            neck_start_x = self.start_pos.x() + arrow_head_length * unit_x_start
            neck_start_y = self.start_pos.y() + arrow_head_length * unit_y_start
        
        # 尾巴起点宽度（单头箭头用）
        tail_width = base_width * 0.15
        
        # 箭杆宽度（在中间点处最宽）
        mid_width = neck_width if is_double else base_shaft_width
        
        # 计算中间点处的方向（使用起点和终点切线的平均）
        avg_unit_x = (unit_x_start + unit_x_end) / 2
        avg_unit_y = (unit_y_start + unit_y_end) / 2
        avg_len = math.sqrt(avg_unit_x * avg_unit_x + avg_unit_y * avg_unit_y)
        if avg_len > 0.01:
            avg_unit_x /= avg_len
            avg_unit_y /= avg_len
        perp_x_mid = -avg_unit_y
        perp_y_mid = avg_unit_x
        
        # 中间点的上下边缘（在曲线上的点）
        mid_upper = QPointF(
            mid_point.x() + perp_x_mid * mid_width / 2,
            mid_point.y() + perp_y_mid * mid_width / 2
        )
        mid_lower = QPointF(
            mid_point.x() - perp_x_mid * mid_width / 2,
            mid_point.y() - perp_y_mid * mid_width / 2
        )
        
        # 构建完整路径
        path = QPainterPath()
        
        if is_bar:
            # === 工字弯曲箭头 ===
            # 三条横杠粗细一致，都使用相同的宽度
            bar_thickness = max(base_width * 1.8, 6)  # 横杠厚度（统一）
            # cap_width 控制“胳膊”左右长度，bar_thickness 控制上下厚度
            cap_width = max(bar_thickness * 2.2, base_width * 3.5)
            shaft_width = bar_thickness  # 中间横杠宽度
            cap_length = min(arrow_head_length * 0.5, max(base_width * 2.0, 8))  # 横杠长度

            start_cap_inner = QPointF(
                self.start_pos.x() + unit_x_start * cap_length,
                self.start_pos.y() + unit_y_start * cap_length
            )
            end_cap_inner = QPointF(
                self.end_pos.x() - unit_x_end * cap_length,
                self.end_pos.y() - unit_y_end * cap_length
            )

            start_cap_left = QPointF(
                self.start_pos.x() + perp_x_start * cap_width,
                self.start_pos.y() + perp_y_start * cap_width
            )
            start_cap_right = QPointF(
                self.start_pos.x() - perp_x_start * cap_width,
                self.start_pos.y() - perp_y_start * cap_width
            )
            start_cap_inner_left = QPointF(
                start_cap_inner.x() + perp_x_start * cap_width,
                start_cap_inner.y() + perp_y_start * cap_width
            )
            start_cap_inner_right = QPointF(
                start_cap_inner.x() - perp_x_start * cap_width,
                start_cap_inner.y() - perp_y_start * cap_width
            )
            end_cap_left = QPointF(
                self.end_pos.x() + perp_x_end * cap_width,
                self.end_pos.y() + perp_y_end * cap_width
            )
            end_cap_right = QPointF(
                self.end_pos.x() - perp_x_end * cap_width,
                self.end_pos.y() - perp_y_end * cap_width
            )
            end_cap_inner_left = QPointF(
                end_cap_inner.x() + perp_x_end * cap_width,
                end_cap_inner.y() + perp_y_end * cap_width
            )
            end_cap_inner_right = QPointF(
                end_cap_inner.x() - perp_x_end * cap_width,
                end_cap_inner.y() - perp_y_end * cap_width
            )

            start_inner_upper = QPointF(
                start_cap_inner.x() + perp_x_start * shaft_width / 2,
                start_cap_inner.y() + perp_y_start * shaft_width / 2
            )
            start_inner_lower = QPointF(
                start_cap_inner.x() - perp_x_start * shaft_width / 2,
                start_cap_inner.y() - perp_y_start * shaft_width / 2
            )
            end_inner_upper = QPointF(
                end_cap_inner.x() + perp_x_end * shaft_width / 2,
                end_cap_inner.y() + perp_y_end * shaft_width / 2
            )
            end_inner_lower = QPointF(
                end_cap_inner.x() - perp_x_end * shaft_width / 2,
                end_cap_inner.y() - perp_y_end * shaft_width / 2
            )

            mid_upper = QPointF(
                mid_point.x() + perp_x_mid * shaft_width / 2,
                mid_point.y() + perp_y_mid * shaft_width / 2
            )
            mid_lower = QPointF(
                mid_point.x() - perp_x_mid * shaft_width / 2,
                mid_point.y() - perp_y_mid * shaft_width / 2
            )

            bezier_upper = QPointF(
                2 * mid_upper.x() - 0.5 * start_inner_upper.x() - 0.5 * end_inner_upper.x(),
                2 * mid_upper.y() - 0.5 * start_inner_upper.y() - 0.5 * end_inner_upper.y()
            )
            bezier_lower = QPointF(
                2 * mid_lower.x() - 0.5 * start_inner_lower.x() - 0.5 * end_inner_lower.x(),
                2 * mid_lower.y() - 0.5 * start_inner_lower.y() - 0.5 * end_inner_lower.y()
            )

            path.moveTo(start_cap_left)
            path.lineTo(start_cap_right)
            path.lineTo(start_cap_inner_right)
            path.lineTo(start_inner_lower)
            path.quadTo(bezier_lower, end_inner_lower)
            path.lineTo(end_cap_inner_right)
            path.lineTo(end_cap_right)
            path.lineTo(end_cap_left)
            path.lineTo(end_cap_inner_left)
            path.lineTo(end_inner_upper)
            path.quadTo(bezier_upper, start_inner_upper)
            path.lineTo(start_cap_inner_left)

        elif is_double:
            # === 双头弯曲箭头 ===
            # 起点颈部的上下边缘
            neck_start_upper = QPointF(
                neck_start_x + perp_x_start * neck_width / 2,
                neck_start_y + perp_y_start * neck_width / 2
            )
            neck_start_lower = QPointF(
                neck_start_x - perp_x_start * neck_width / 2,
                neck_start_y - perp_y_start * neck_width / 2
            )
            
            # 终点颈部的上下边缘
            neck_end_upper = QPointF(
                neck_end_x + perp_x_end * neck_width / 2,
                neck_end_y + perp_y_end * neck_width / 2
            )
            neck_end_lower = QPointF(
                neck_end_x - perp_x_end * neck_width / 2,
                neck_end_y - perp_y_end * neck_width / 2
            )
            
            # 计算上边缘的贝塞尔控制点（让曲线穿过 mid_upper）
            bezier_upper = QPointF(
                2 * mid_upper.x() - 0.5 * neck_start_upper.x() - 0.5 * neck_end_upper.x(),
                2 * mid_upper.y() - 0.5 * neck_start_upper.y() - 0.5 * neck_end_upper.y()
            )
            
            # 计算下边缘的贝塞尔控制点（让曲线穿过 mid_lower）
            bezier_lower = QPointF(
                2 * mid_lower.x() - 0.5 * neck_start_lower.x() - 0.5 * neck_end_lower.x(),
                2 * mid_lower.y() - 0.5 * neck_start_lower.y() - 0.5 * neck_end_lower.y()
            )
            
            # 起点箭头左翼
            start_wing_left = QPointF(
                neck_start_x + perp_x_start * arrow_head_width,
                neck_start_y + perp_y_start * arrow_head_width
            )
            path.moveTo(start_wing_left)
            
            # 起点尖端
            path.lineTo(self.start_pos.x(), self.start_pos.y())
            
            # 起点箭头右翼
            start_wing_right = QPointF(
                neck_start_x - perp_x_start * arrow_head_width,
                neck_start_y - perp_y_start * arrow_head_width
            )
            path.lineTo(start_wing_right)
            
            # 起点箭头凹陷效果
            # 双头弯曲箭头：减小凹陷深度，避免连接处缺口
            start_notch_depth = arrow_head_length * 0.05
            start_notch = QPointF(
                neck_start_x + unit_x_start * start_notch_depth,
                neck_start_y + unit_y_start * start_notch_depth
            )
            path.quadTo(start_notch, neck_start_lower)
            
            # 下半边曲线（从起点颈部到终点颈部）
            path.quadTo(bezier_lower, neck_end_lower)
            
            # 终点箭头右翼
            end_wing_right = QPointF(
                neck_end_x - perp_x_end * arrow_head_width,
                neck_end_y - perp_y_end * arrow_head_width
            )
            path.lineTo(end_wing_right)
            
            # 终点尖端
            path.lineTo(self.end_pos.x(), self.end_pos.y())
            
            # 终点箭头左翼
            end_wing_left = QPointF(
                neck_end_x + perp_x_end * arrow_head_width,
                neck_end_y + perp_y_end * arrow_head_width
            )
            path.lineTo(end_wing_left)
            
            # 终点箭头凹陷效果
            # 双头弯曲箭头：减小凹陷深度，避免连接处缺口
            end_notch_depth = arrow_head_length * 0.05
            end_notch = QPointF(
                neck_end_x - unit_x_end * end_notch_depth,
                neck_end_y - unit_y_end * end_notch_depth
            )
            path.quadTo(end_notch, neck_end_upper)
            
            # 上半边曲线（从终点颈部回到起点颈部）
            path.quadTo(bezier_upper, neck_start_upper)
            
        else:
            # === 单头弯曲箭头（原版算法） ===
            # 计算上边缘的贝塞尔控制点（让曲线穿过 mid_upper）
            start_upper = QPointF(
                self.start_pos.x() + perp_x_start * tail_width / 2,
                self.start_pos.y() + perp_y_start * tail_width / 2
            )
            neck_upper = QPointF(
                neck_end_x + perp_x_end * neck_width / 2,
                neck_end_y + perp_y_end * neck_width / 2
            )
            # P1 = 2*M - 0.5*P0 - 0.5*P2
            bezier_upper = QPointF(
                2 * mid_upper.x() - 0.5 * start_upper.x() - 0.5 * neck_upper.x(),
                2 * mid_upper.y() - 0.5 * start_upper.y() - 0.5 * neck_upper.y()
            )
            
            # 计算下边缘的贝塞尔控制点（让曲线穿过 mid_lower）
            start_lower = QPointF(
                self.start_pos.x() - perp_x_start * tail_width / 2,
                self.start_pos.y() - perp_y_start * tail_width / 2
            )
            neck_lower = QPointF(
                neck_end_x - perp_x_end * neck_width / 2,
                neck_end_y - perp_y_end * neck_width / 2
            )
            bezier_lower = QPointF(
                2 * mid_lower.x() - 0.5 * start_lower.x() - 0.5 * neck_lower.x(),
                2 * mid_lower.y() - 0.5 * start_lower.y() - 0.5 * neck_lower.y()
            )
            
            # === 上半边（从起点到颈部） ===
            path.moveTo(start_upper)
            path.quadTo(bezier_upper, neck_upper)
            
            # === 箭头三角形部分 ===
            # 左翼
            wing_left = QPointF(
                neck_end_x + perp_x_end * arrow_head_width,
                neck_end_y + perp_y_end * arrow_head_width
            )
            path.lineTo(wing_left)
            
            # 箭头尖端
            path.lineTo(self.end_pos.x(), self.end_pos.y())
            
            # 右翼
            wing_right = QPointF(
                neck_end_x - perp_x_end * arrow_head_width,
                neck_end_y - perp_y_end * arrow_head_width
            )
            path.lineTo(wing_right)
            
            # 后弯曲效果（箭头凹陷）
            # 限制凹陷深度,避免短箭头自交导致白洞
            notch_depth = min(
                arrow_head_length * 0.05,  # 降低凹陷比例,与双头箭头保持一致
                neck_width * 0.4           # 最大不超过颈部宽度的 40%
            )
            notch_x = neck_end_x - unit_x_end * notch_depth
            notch_y = neck_end_y - unit_y_end * notch_depth
            
            path.quadTo(QPointF(notch_x, notch_y), neck_lower)
            
            # === 下半边（从颈部回到起点） ===
            path.quadTo(bezier_lower, start_lower)
        
        path.closeSubpath()
        
        self.setPath(path)
    
    def paint(self, painter, option, widget=None):
        """优化渲染"""
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(self.color)
        painter.drawPath(self.path())

        if (self.isSelected() or self._hovered) and self._can_show_hover():
            selection_pen = QPen(QColor(0, 180, 255, 230), 2, Qt.PenStyle.DashLine)
            selection_pen.setCosmetic(True)
            selection_pen.setCapStyle(Qt.PenCapStyle.RoundCap)
            selection_pen.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
            painter.setBrush(Qt.BrushStyle.NoBrush)
            painter.setPen(selection_pen)
            painter.drawPath(self.path())

    def hoverEnterEvent(self, event):
        if not self._can_show_hover():
            self._hovered = False
            self.update()
            self.unsetCursor()
            event.accept()
            return
        self._hovered = True
        self.update()
        self.setCursor(Qt.CursorShape.SizeAllCursor)
        event.accept()

    def hoverMoveEvent(self, event):
        if not self._can_show_hover():
            self.unsetCursor()
            event.accept()
            return
        self.setCursor(Qt.CursorShape.SizeAllCursor)
        event.accept()

    def hoverLeaveEvent(self, event):
        self._hovered = False
        self.update()
        self.unsetCursor()
        event.accept()

    # -- 统一属性接口 --

    def set_stroke_width(self, width: float):
        self.base_width = max(1.0, float(width))
        self.update_geometry()
        self.update()

    def scale_stroke_width(self, scale: float) -> bool:
        self.base_width = max(1.0, self.base_width * scale)
        self.update_geometry()
        self.update()
        return True

    def set_visual_opacity(self, opacity: float) -> bool:
        opacity = max(0.0, min(1.0, float(opacity)))
        color = QColor(self.color)
        color.setAlphaF(opacity)
        self.color = color
        self.setOpacity(1.0)
        self.update()
        return True

    def get_stroke_width(self) -> float | None:
        return float(self.base_width)

    def get_visual_opacity(self) -> float | None:
        direct = max(0.0, min(1.0, float(self.opacity())))
        if direct < 0.999:
            return direct
        return self.color.alphaF()


class TextItem(QGraphicsTextItem, DrawingItemMixin):
    """文字图元 - 增强版"""
    # 文字与虚线边框之间的内边距（document margin）
    TEXT_PADDING = 8
    
    def __init__(self, text: str, pos: QPointF, font: QFont, color: QColor):
        super().__init__(text)
        self._init_drawing_mixin()
        self.setPos(pos)
        self.setFont(font)
        self.setDefaultTextColor(color)
        # 允许点击编辑
        self.setTextInteractionFlags(Qt.TextInteractionFlag.TextEditorInteraction)
        self.setZValue(20)
        
        # 增大 document margin，使虚线边框与文字之间有足够间距
        # 默认只有 4px，太小导致鼠标难以区分文字区域和边框区域
        self.document().setDocumentMargin(self.TEXT_PADDING)
        
        # 增强属性
        self.has_outline = True  # 默认开启描边
        self.outline_color = QColor(Qt.GlobalColor.white)
        self.outline_width = 3
        
        self.has_shadow = True   # 默认开启阴影
        self.shadow_color = QColor(0, 0, 0, 100)
        self.shadow_offset = QPointF(2, 2)
        
        self.has_background = False # 默认关闭背景
        self.background_color = QColor(255, 255, 255, 255) # 白色全不透明
        
    def paint(self, painter, option, widget):
        """重写绘制方法以支持描边和阴影"""
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.setRenderHint(QPainter.RenderHint.TextAntialiasing)
        
        # 1. 绘制背景（如果在底层）
        if self.has_background:
            painter.save()
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(self.background_color)
            painter.drawRect(self.boundingRect())
            painter.restore()
            
        # 2. 绘制描边 (Outline) - 使用路径绘制法，效果最好
        if self.has_outline:
            painter.save()
            # 获取文字路径
            path = QPainterPath()
            # 注意：addText 的位置需要微调以匹配 QGraphicsTextItem 的内部边距
            # 默认边距通常是 4px 左右，但这取决于字体
            # 更精确的方法是遍历 layout，但这里我们用一个经验值
            margin = 0 # QGraphicsTextItem 默认没有 margin，但 document 有
            # 实际上 QGraphicsTextItem 的绘制起点就是 (0,0)
            
            # 使用 QPainterPath 绘制文字轮廓
            # 注意：toPlainText() 获取的是纯文本，如果有多行需要处理
            # 这里简化处理：假设是单行或简单多行
            # 为了完美对齐，我们应该使用 document 的 layout
            
            # 简易版描边：只对纯文本有效
            # 这种方法在编辑时可能会有轻微错位，但在展示时效果很好
            # 为了避免错位，我们只在非编辑状态或简单文本时启用？
            # 不，我们尝试对齐。
            
            # 更好的方法：绘制 8 次偏移（性能稍差但绝对对齐）
            # 这种方法兼容所有富文本格式
            steps = 8
            import math
            
            # 保存原始画笔
            original_pen = painter.pen()
            
            # 设置描边画笔
            painter.setPen(self.outline_color)
            
            # 绘制 8 个方向的偏移
            offset = self.outline_width / 1.5
            for i in range(steps):
                angle = 2 * math.pi * i / steps
                dx = math.cos(angle) * offset
                dy = math.sin(angle) * offset
                
                painter.save()
                painter.translate(dx, dy)

                painter.restore()
            

            path = QPainterPath()
            font = self.font()

            painter.setBrush(Qt.BrushStyle.NoBrush) # 不填充
            pass
            painter.restore()

        # 3. 绘制阴影
        if self.has_shadow:
            # 简单阴影：绘制一个半透明的背景框偏移
            pass

        # 调用原始绘制（绘制文本本身、光标、选区）
        super().paint(painter, option, widget)
        
    def set_outline(self, enabled: bool, color: QColor = None, width: int = 3):
        self.has_outline = enabled
        if color: self.outline_color = color
        self.outline_width = width
        self.update()
        
    def set_shadow(self, enabled: bool, color: QColor = None):
        self.has_shadow = enabled
        if color: self.shadow_color = color
        self.update()
        
    def set_background(self, enabled: bool, color: QColor = None, opacity: int = None):
        self.has_background = enabled
        if color:
            self.background_color = QColor(color)
        if opacity is not None:
            self.background_color.setAlpha(int(max(0, min(255, opacity))))
        self.update()
        
    @safe_event
    def focusOutEvent(self, event):
        """失去焦点时，如果内容为空则自动删除"""
        super().focusOutEvent(event)
        # 移除选中状态
        cursor = self.textCursor()
        cursor.clearSelection()
        self.setTextCursor(cursor)
        
        # 如果内容为空，删除自己
        if not self.toPlainText().strip():
            if self.scene():
                self.scene().removeItem(self)
                print("[TextItem] 内容为空，自动删除")
        else:
            # 否则取消编辑模式（可选）
            self.setTextInteractionFlags(Qt.TextInteractionFlag.NoTextInteraction)
            # 恢复为可选择
            self.setFlag(QGraphicsItem.GraphicsItemFlag.ItemIsSelectable, True)
            
    @safe_event
    def mouseDoubleClickEvent(self, event):
        """双击进入编辑模式"""
        if self.textInteractionFlags() == Qt.TextInteractionFlag.NoTextInteraction:
            self.setTextInteractionFlags(Qt.TextInteractionFlag.TextEditorInteraction)
            self.setFocus()
        super().mouseDoubleClickEvent(event)

    def _is_on_text_edge(self, local_pos: QPointF) -> bool:
        """
        判断局部坐标是否在边框边缘（内边距区域）
        在内边距区域 → True（应显示拖拽光标）
        在文字内容区域 → False（应显示文字编辑光标）
        """
        rect = self.boundingRect()
        if not rect.contains(local_pos):
            return False
        margin = self.document().documentMargin()
        inner = rect.adjusted(margin, margin, -margin, -margin)
        if inner.width() <= 0 or inner.height() <= 0:
            return True
        return not inner.contains(local_pos)

    def hoverEnterEvent(self, event):
        """重写悬停进入：编辑模式下区分文字区域和边缘区域的光标"""
        if self.textInteractionFlags() & Qt.TextInteractionFlag.TextEditorInteraction:
            if self._is_on_text_edge(event.pos()):
                self.setCursor(Qt.CursorShape.SizeAllCursor)
            else:
                self.setCursor(Qt.CursorShape.IBeamCursor)
            event.accept()
        else:
            super().hoverEnterEvent(event)

    def hoverMoveEvent(self, event):
        """重写悬停移动：编辑模式下区分文字区域和边缘区域的光标"""
        if self.textInteractionFlags() & Qt.TextInteractionFlag.TextEditorInteraction:
            if self._is_on_text_edge(event.pos()):
                self.setCursor(Qt.CursorShape.SizeAllCursor)
            else:
                self.setCursor(Qt.CursorShape.IBeamCursor)
            event.accept()
        else:
            super().hoverMoveEvent(event)

    def hoverLeaveEvent(self, event):
        """重写悬停离开：恢复光标"""
        self.unsetCursor()
        event.accept()

    # -- 统一属性接口 --

    def scale_stroke_width(self, scale: float) -> bool:
        """对文字图元，缩放字号"""
        font = self.font()
        point_size = font.pointSizeF()
        if point_size <= 0:
            point_size = float(font.pointSize() or 12)
        new_size = max(6.0, point_size * scale)
        font.setPointSizeF(new_size)
        self.setFont(font)
        self.update()
        return True

    def set_visual_opacity(self, opacity: float) -> bool:
        opacity = max(0.0, min(1.0, float(opacity)))
        self.setOpacity(opacity)
        self.update()
        return True

    def get_visual_opacity(self) -> float | None:
        return max(0.0, min(1.0, float(self.opacity())))


class NumberItem(QGraphicsItem, DrawingItemMixin):
    """序号图元"""
    FONT_SCALE = 0.95
    MIN_FONT_SIZE = 10
    HOVER_OUTLINE_WIDTH = 2

    def __init__(self, number: int, pos: QPointF, radius: float, color: QColor):
        super().__init__()
        self._init_drawing_mixin()
        self.number = number
        self.radius = radius
        self.color = color
        self.setPos(pos)
        self.setZValue(20)
        self._hovered = False
        
    def boundingRect(self):
        return QRectF(-self.radius, -self.radius, self.radius*2, self.radius*2)

    def paint(self, painter, option, widget):
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        
        # 绘制背景圆
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(self.color)
        painter.drawEllipse(self.boundingRect())
        
        # 绘制数字：根据背景色亮度选择黑或白文字（整数权重快速判定）
        try:
            bg_color = self.color if self.color is not None else QColor(0, 0, 0)
            r = int(bg_color.red())
            g = int(bg_color.green())
            b = int(bg_color.blue())
            # 使用整数权重：Y = (R*3 + G*6 + B*1) / 10
            # 为避免浮点，比较放大后的值（阈值 128 -> 128*10）
            y_scaled = r * 3 + g * 6 + b * 1
            if y_scaled > 128 * 10:
                text_color = QColor(0, 0, 0)
            else:
                text_color = QColor(255, 255, 255)
        except Exception:
            # 任何异常都回退到白色文字，保证鲁棒性
            text_color = QColor(255, 255, 255)

        painter.setPen(text_color)
        font_size = max(self.MIN_FONT_SIZE, int(self.radius * self.FONT_SCALE))
        # 创建字体时不使用QFont.Weight.Bold，改用setBold避免字体变体问题
        font = QFont("Arial", font_size)
        font.setBold(True)
        painter.setFont(font)
        painter.drawText(self.boundingRect(), Qt.AlignmentFlag.AlignCenter, str(self.number))

        if (self.isSelected() or self._hovered) and self._can_show_hover():
            outline_pen = QPen(QColor(0, 180, 255, 230), self.HOVER_OUTLINE_WIDTH, Qt.PenStyle.DashLine)
            outline_pen.setCosmetic(True)
            painter.setBrush(Qt.BrushStyle.NoBrush)
            painter.setPen(outline_pen)
            painter.drawEllipse(self.boundingRect())

    def hoverEnterEvent(self, event):
        if not self._can_show_hover():
            self._hovered = False
            self.update()
            self.unsetCursor()
            event.accept()
            return
        self._hovered = True
        self.update()
        self.setCursor(Qt.CursorShape.SizeAllCursor)
        event.accept()

    def hoverMoveEvent(self, event):
        if not self._can_show_hover():
            self.unsetCursor()
            event.accept()
            return
        self.setCursor(Qt.CursorShape.SizeAllCursor)
        event.accept()

    def hoverLeaveEvent(self, event):
        self._hovered = False
        self.update()
        self.unsetCursor()
        event.accept()

    # -- 统一属性接口 --

    def set_stroke_width(self, width: float):
        """对序号图元，width 映射为 radius"""
        from tools.number import NumberTool
        self.radius = max(4.0, float(width) * NumberTool.RADIUS_SCALE)
        self.update()

    def scale_stroke_width(self, scale: float) -> bool:
        self.radius = max(4.0, self.radius * scale)
        self.update()
        return True

    def set_visual_opacity(self, opacity: float) -> bool:
        opacity = max(0.0, min(1.0, float(opacity)))
        color = QColor(self.color)
        color.setAlphaF(opacity)
        self.color = color
        self.setOpacity(1.0)
        self.update()
        return True

    def get_stroke_width(self) -> float | None:
        from tools.number import NumberTool
        if NumberTool.RADIUS_SCALE <= 0:
            return float(self.radius)
        return float(self.radius / NumberTool.RADIUS_SCALE)

    def get_visual_opacity(self) -> float | None:
        direct = max(0.0, min(1.0, float(self.opacity())))
        if direct < 0.999:
            return direct
        return self.color.alphaF()
 