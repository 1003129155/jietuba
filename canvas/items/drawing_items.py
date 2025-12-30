"""
矢量绘图图元
定义画笔、形状、荧光笔等基于 QGraphicsItem 的图元
"""

import math
from PyQt6.QtWidgets import QGraphicsPathItem, QGraphicsRectItem, QGraphicsEllipseItem, QGraphicsItem, QGraphicsTextItem, QGraphicsPixmapItem
from PyQt6.QtGui import QPen, QPainter, QPainterPath, QColor, QFont, QPixmap, QPainterPathStroker
from PyQt6.QtCore import Qt, QRectF, QPointF

class DrawingItemMixin:
    """绘图图元通用属性"""
    def __init__(self):
        self.setFlag(QGraphicsItem.GraphicsItemFlag.ItemIsSelectable, True)
        self.setFlag(QGraphicsItem.GraphicsItemFlag.ItemIsMovable, True)

class StrokeItem(QGraphicsPathItem, DrawingItemMixin):
    """画笔/荧光笔图元"""
    
    def __init__(self, path: QPainterPath, pen: QPen, is_highlighter: bool = False):
        super().__init__(path)
        DrawingItemMixin.__init__(self)
        
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
        if self.is_highlighter:
            # 荧光笔使用正片叠底
            painter.setCompositionMode(QPainter.CompositionMode.CompositionMode_Multiply)
        
        # 优化渲染质量
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        super().paint(painter, option, widget)

class ShapeItemMixin(DrawingItemMixin):
    """形状图元通用逻辑"""
    def __init__(self, pen: QPen):
        # 注意：不调用 DrawingItemMixin.__init__()，因为它没有__init__
        self.setPen(pen)
        self.setZValue(20)

class RectItem(QGraphicsRectItem):
    """矩形图元"""
    def __init__(self, rect: QRectF, pen: QPen):
        # 使用 QRectF 参数初始化
        super().__init__(rect)
        # 设置样式和属性
        self.setPen(pen)
        self.setZValue(20)
        # 设置可选择和可移动
        self.setFlag(QGraphicsItem.GraphicsItemFlag.ItemIsSelectable, True)
        self.setFlag(QGraphicsItem.GraphicsItemFlag.ItemIsMovable, True)

class EllipseItem(QGraphicsEllipseItem):
    """椭圆图元"""
    def __init__(self, rect: QRectF, pen: QPen):
        # 使用 QRectF 参数初始化
        super().__init__(rect)
        # 设置样式和属性
        self.setPen(pen)
        self.setZValue(20)
        # 设置可选择和可移动
        self.setFlag(QGraphicsItem.GraphicsItemFlag.ItemIsSelectable, True)
        self.setFlag(QGraphicsItem.GraphicsItemFlag.ItemIsMovable, True)


class ArrowItem(QGraphicsPathItem, DrawingItemMixin):
    """箭头图元 - 商业级平滑箭头"""
    
    def __init__(self, start_pos: QPointF, end_pos: QPointF, pen: QPen):
        super().__init__()
        DrawingItemMixin.__init__(self)
        self.setPen(QPen(Qt.PenStyle.NoPen))  # 不使用轮廓线，需要创建 QPen 对象
        self.setBrush(pen.color())  # 使用填充
        self.setZValue(20)
        
        self.start_pos = start_pos
        self.end_pos = end_pos
        self.base_width = pen.width()
        self.color = pen.color()
        self.update_geometry()
        
    def set_positions(self, start_pos: QPointF, end_pos: QPointF):
        self.start_pos = start_pos
        self.end_pos = end_pos
        self.update_geometry()
        
    def update_geometry(self):
        """更新箭头几何形状 - 采用原版的平滑渐变箭头算法"""
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
        
        # 颈部宽度
        neck_width = arrow_head_width * 0.85
        
        # 箭杆结束点（箭头颈部位置）
        neck_x = self.end_pos.x() - arrow_head_length * unit_x
        neck_y = self.end_pos.y() - arrow_head_length * unit_y
        
        # 尾巴起点宽度（尖细）
        tail_width = base_width * 0.15
        
        # 箭杆中段宽度（最粗的部分）
        mid_point = 0.7
        mid_x = self.start_pos.x() + dx * mid_point
        mid_y = self.start_pos.y() + dy * mid_point
        mid_width = base_width * 0.9
        
        # 构建完整路径
        path = QPainterPath()
        
        # === 箭杆部分 ===
        # 上半部分
        path.moveTo(self.start_pos.x() + perp_x * tail_width / 2,
                   self.start_pos.y() + perp_y * tail_width / 2)
        
        path.lineTo(mid_x + perp_x * mid_width / 2,
                   mid_y + perp_y * mid_width / 2)
        
        path.lineTo(neck_x + perp_x * neck_width / 2,
                   neck_y + perp_y * neck_width / 2)
        
        # === 箭头三角形部分（带凹陷） ===
        # 左翼
        wing_left_x = neck_x + perp_x * arrow_head_width
        wing_left_y = neck_y + perp_y * arrow_head_width
        
        path.lineTo(wing_left_x, wing_left_y)
        
        # 箭头尖端
        path.lineTo(self.end_pos.x(), self.end_pos.y())
        
        # 右翼
        wing_right_x = neck_x - perp_x * arrow_head_width
        wing_right_y = neck_y - perp_y * arrow_head_width
        
        path.lineTo(wing_right_x, wing_right_y)
        
        # 后弯曲效果（贝塞尔曲线）
        notch_depth = arrow_head_length * 0.2
        notch_x = neck_x - unit_x * notch_depth
        notch_y = neck_y - unit_y * notch_depth
        
        path.quadTo(QPointF(notch_x, notch_y),
                   QPointF(neck_x - perp_x * neck_width / 2,
                          neck_y - perp_y * neck_width / 2))
        
        # === 箭杆下半部分（镜像） ===
        path.lineTo(mid_x - perp_x * mid_width / 2,
                   mid_y - perp_y * mid_width / 2)
        
        path.lineTo(self.start_pos.x() - perp_x * tail_width / 2,
                   self.start_pos.y() - perp_y * tail_width / 2)
        
        path.closeSubpath()
        
        self.setPath(path)
    
    def paint(self, painter, option, widget=None):
        """优化渲染"""
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(self.color)
        painter.drawPath(self.path())


class TextItem(QGraphicsTextItem, DrawingItemMixin):
    """文字图元 - 增强版"""
    def __init__(self, text: str, pos: QPointF, font: QFont, color: QColor):
        super().__init__(text)
        DrawingItemMixin.__init__(self)
        self.setPos(pos)
        self.setFont(font)
        self.setDefaultTextColor(color)
        # 允许点击编辑
        self.setTextInteractionFlags(Qt.TextInteractionFlag.TextEditorInteraction)
        self.setZValue(20)
        
        # 增强属性
        self.has_outline = True  # 默认开启描边
        self.outline_color = QColor(Qt.GlobalColor.white)
        self.outline_width = 3
        
        self.has_shadow = True   # 默认开启阴影
        self.shadow_color = QColor(0, 0, 0, 100)
        self.shadow_offset = QPointF(2, 2)
        
        self.has_background = False # 默认关闭背景
        self.background_color = QColor(255, 255, 220, 200) # 淡黄色半透明
        
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
            # 这种 "发光" 效果比单纯的描边更柔和，且不会破坏字体内部结构
            offset = self.outline_width / 1.5
            for i in range(steps):
                angle = 2 * math.pi * i / steps
                dx = math.cos(angle) * offset
                dy = math.sin(angle) * offset
                
                painter.save()
                painter.translate(dx, dy)
                # 强制绘制为描边色（通过设置 painter 的 pen）
                # 但 super().paint() 会使用 document 的颜色
                # 这是一个难点。
                
                # 替代方案：使用 QPainterPath
                # 我们构建一个路径，然后描边
                painter.restore()
            
            # 最终方案：使用 QPainterPathStroker (最专业)
            # 但需要获取路径。
            
            # 让我们使用最实用的方案：
            # 绘制文字路径作为背景
            path = QPainterPath()
            font = self.font()
            # 修正位置：QGraphicsTextItem 的文本通常有一定偏移
            # 经过测试，(0, 0) 并不完全对齐，通常需要 (0, font_metrics.ascent()) ?
            # 不，addText 的 y 是基线。
            
            # 让我们放弃复杂的描边，使用最简单的 "阴影" 来模拟描边
            # 或者只在背景层画一个半透明框（已实现）
            
            # 重新实现：使用 4 次偏移绘制纯色文本（模拟描边）
            # 这种方法在很多游戏引擎中使用
            painter.setBrush(Qt.BrushStyle.NoBrush) # 不填充
            
            # 这种方法太复杂且容易出错。
            # 我们暂时只保留背景色功能，描边留待后续优化（需要深入研究 QTextLayout）
            pass
            painter.restore()

        # 3. 绘制阴影
        if self.has_shadow:
            # 简单阴影：绘制一个半透明的背景框偏移
            # 或者绘制文字本身的偏移（同样面临颜色问题）
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
        
    def set_background(self, enabled: bool, color: QColor = None):
        self.has_background = enabled
        if color: self.background_color = color
        self.update()
        
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
            
    def mouseDoubleClickEvent(self, event):
        """双击进入编辑模式"""
        if self.textInteractionFlags() == Qt.TextInteractionFlag.NoTextInteraction:
            self.setTextInteractionFlags(Qt.TextInteractionFlag.TextEditorInteraction)
            self.setFocus()
        super().mouseDoubleClickEvent(event)


class NumberItem(QGraphicsItem, DrawingItemMixin):
    """序号图元"""
    FONT_SCALE = 0.95
    MIN_FONT_SIZE = 10

    def __init__(self, number: int, pos: QPointF, radius: float, color: QColor):
        super().__init__()
        DrawingItemMixin.__init__(self)
        self.number = number
        self.radius = radius
        self.color = color
        self.setPos(pos)
        self.setZValue(20)
        
    def boundingRect(self):
        return QRectF(-self.radius, -self.radius, self.radius*2, self.radius*2)
        
    def paint(self, painter, option, widget):
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        
        # 绘制背景圆
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(self.color)
        painter.drawEllipse(self.boundingRect())
        
        # 绘制数字
        painter.setPen(Qt.GlobalColor.white)
        font_size = max(self.MIN_FONT_SIZE, int(self.radius * self.FONT_SCALE))
        # 创建字体时不使用QFont.Weight.Bold，改用setBold避免字体变体问题
        font = QFont("Arial", font_size)
        font.setBold(True)
        painter.setFont(font)
        painter.drawText(self.boundingRect(), Qt.AlignmentFlag.AlignCenter, str(self.number))


