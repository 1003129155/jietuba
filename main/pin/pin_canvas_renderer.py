"""
钉图画布 - 矢量命令渲染器
负责将矢量绘图命令渲染到 QPainter
"""

from PyQt6.QtCore import Qt, QPointF, QRectF
from PyQt6.QtGui import QPainter, QPen, QBrush, QColor, QPainterPath
from core import log_warning


class VectorCommandRenderer:
    """
    矢量命令渲染器
    
    负责将矢量绘图命令渲染到 QPainter，支持：
    - 画笔（pen）
    - 矩形（rect）
    - 椭圆（ellipse）
    - 箭头（arrow）
    - 文字（text）
    - 荧光笔（highlighter）
    - 序号（number）
    """
    
    _instance = None
    
    @classmethod
    def instance(cls) -> 'VectorCommandRenderer':
        """获取单例实例"""
        if cls._instance is None:
            cls._instance = VectorCommandRenderer()
        return cls._instance
    
    def __init__(self):
        pass
    
    def render_commands(self, painter: QPainter, commands: list, target_rect: QRectF = None):
        """
        渲染所有矢量命令
        
        Args:
            painter: QPainter 对象
            commands: 矢量命令列表
            target_rect: 目标矩形（用于缩放坐标变换）
        """
        if not commands:
            return
        
        # 保存 painter 状态
        painter.save()
        
        # 如果提供了 target_rect，可以在这里做坐标变换
        # 目前假设命令坐标已经是相对于选区的
        
        # 渲染每个命令
        for cmd in commands:
            try:
                self._render_command(painter, cmd)
            except Exception as e:
                log_warning(f"渲染命令失败: {e}, 命令: {cmd.get('type', 'unknown')}", "Renderer")
        
        # 恢复 painter 状态
        painter.restore()
    
    def _render_command(self, painter: QPainter, cmd: dict):
        """
        渲染单个命令
        
        Args:
            painter: QPainter 对象
            cmd: 命令字典
        """
        cmd_type = cmd.get('type')
        
        if cmd_type == 'pen':
            self._render_pen(painter, cmd)
        elif cmd_type == 'rect':
            self._render_rect(painter, cmd)
        elif cmd_type == 'ellipse':
            self._render_ellipse(painter, cmd)
        elif cmd_type == 'arrow':
            self._render_arrow(painter, cmd)
        elif cmd_type == 'text':
            self._render_text(painter, cmd)
        elif cmd_type == 'highlighter':
            self._render_highlighter(painter, cmd)
        elif cmd_type == 'number':
            self._render_number(painter, cmd)
        else:
            log_warning(f"未知命令类型: {cmd_type}", "Renderer")
    
    # ==================== 具体渲染方法 ====================
    
    def _render_pen(self, painter: QPainter, cmd: dict):
        """渲染画笔路径"""
        points = cmd.get('points', [])
        if len(points) < 2:
            return
        
        color = cmd.get('color', QColor(255, 0, 0))
        width = cmd.get('width', 3)
        
        pen = QPen(color, width, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap, Qt.PenJoinStyle.RoundJoin)
        painter.setPen(pen)
        painter.setBrush(Qt.BrushStyle.NoBrush)
        
        # 绘制路径
        path = QPainterPath()
        path.moveTo(points[0])
        for point in points[1:]:
            path.lineTo(point)
        
        painter.drawPath(path)
    
    def _render_rect(self, painter: QPainter, cmd: dict):
        """渲染矩形"""
        rect = cmd.get('rect')
        if not rect or rect.isEmpty():
            return
        
        color = cmd.get('color', QColor(255, 0, 0))
        width = cmd.get('width', 3)
        filled = cmd.get('filled', False)
        
        pen = QPen(color, width, Qt.PenStyle.SolidLine)
        painter.setPen(pen)
        
        if filled:
            brush = QBrush(color)
            painter.setBrush(brush)
        else:
            painter.setBrush(Qt.BrushStyle.NoBrush)
        
        painter.drawRect(rect)
    
    def _render_ellipse(self, painter: QPainter, cmd: dict):
        """渲染椭圆"""
        rect = cmd.get('rect')
        if not rect or rect.isEmpty():
            return
        
        color = cmd.get('color', QColor(255, 0, 0))
        width = cmd.get('width', 3)
        filled = cmd.get('filled', False)
        
        pen = QPen(color, width, Qt.PenStyle.SolidLine)
        painter.setPen(pen)
        
        if filled:
            brush = QBrush(color)
            painter.setBrush(brush)
        else:
            painter.setBrush(Qt.BrushStyle.NoBrush)
        
        painter.drawEllipse(rect)
    
    def _render_arrow(self, painter: QPainter, cmd: dict):
        """渲染箭头"""
        start = cmd.get('start')
        end = cmd.get('end')
        if not start or not end:
            return
        
        color = cmd.get('color', QColor(255, 0, 0))
        width = cmd.get('width', 3)
        
        pen = QPen(color, width, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap)
        painter.setPen(pen)
        painter.setBrush(QBrush(color))
        
        # 绘制箭头主线
        painter.drawLine(start, end)
        
        # 绘制箭头三角形
        arrow_head = self._create_arrow_head(start, end, width * 2)
        if arrow_head:
            painter.drawPolygon(arrow_head)
    
    def _render_text(self, painter: QPainter, cmd: dict):
        """渲染文字"""
        text = cmd.get('text', '')
        if not text:
            return
        
        pos = cmd.get('pos', QPointF(0, 0))
        font = cmd.get('font')
        color = cmd.get('color', QColor(255, 0, 0))
        
        if font:
            painter.setFont(font)
        
        pen = QPen(color)
        painter.setPen(pen)
        painter.setBrush(Qt.BrushStyle.NoBrush)
        
        painter.drawText(pos, text)
    
    def _render_highlighter(self, painter: QPainter, cmd: dict):
        """渲染荧光笔（半透明路径）"""
        points = cmd.get('points', [])
        if len(points) < 2:
            return
        
        color = cmd.get('color', QColor(255, 255, 0, 100))  # 半透明黄色
        width = cmd.get('width', 20)
        
        pen = QPen(color, width, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap, Qt.PenJoinStyle.RoundJoin)
        painter.setPen(pen)
        painter.setBrush(Qt.BrushStyle.NoBrush)
        
        # 设置混合模式（荧光笔效果）
        painter.setCompositionMode(QPainter.CompositionMode.CompositionMode_Multiply)
        
        # 绘制路径
        path = QPainterPath()
        path.moveTo(points[0])
        for point in points[1:]:
            path.lineTo(point)
        
        painter.drawPath(path)
        
        # 恢复混合模式
        painter.setCompositionMode(QPainter.CompositionMode.CompositionMode_SourceOver)
    
    def _render_number(self, painter: QPainter, cmd: dict):
        """渲染序号（圆圈+数字）"""
        pos = cmd.get('pos', QPointF(0, 0))
        number = cmd.get('number', 1)
        color = cmd.get('color', QColor(255, 0, 0))
        size = cmd.get('size', 30)
        
        # 绘制圆圈
        pen = QPen(color, 3)
        painter.setPen(pen)
        painter.setBrush(QBrush(color))
        
        circle_rect = QRectF(pos.x() - size/2, pos.y() - size/2, size, size)
        painter.drawEllipse(circle_rect)
        
        # 绘制数字（白色）
        painter.setPen(QPen(Qt.GlobalColor.white))
        font = painter.font()
        font.setPixelSize(int(size * 0.6))
        font.setBold(True)
        painter.setFont(font)
        
        painter.drawText(circle_rect, Qt.AlignmentFlag.AlignCenter, str(number))
    
    # ==================== 辅助方法 ====================
    
    def _create_arrow_head(self, start: QPointF, end: QPointF, size: float):
        """
        创建箭头三角形
        
        Args:
            start: 起点
            end: 终点
            size: 箭头大小
        
        Returns:
            QPolygonF: 箭头三角形
        """
        from PyQt6.QtGui import QPolygonF, QTransform
        from math import atan2, degrees
        
        # 计算角度
        dx = end.x() - start.x()
        dy = end.y() - start.y()
        
        if dx == 0 and dy == 0:
            return None
        
        angle = degrees(atan2(dy, dx))
        
        # 创建箭头形状（指向右）
        arrow = QPolygonF([
            QPointF(0, 0),
            QPointF(-size, size/2),
            QPointF(-size, -size/2)
        ])
        
        # 变换到终点位置和角度
        transform = QTransform()
        transform.translate(end.x(), end.y())
        transform.rotate(angle)
        
        return transform.map(arrow)


# 便捷函数（保持向后兼容）
def get_vector_renderer() -> VectorCommandRenderer:
    """获取全局矢量渲染器实例"""
    return VectorCommandRenderer.instance()
