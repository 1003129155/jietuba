"""
遮罩层 - 挖洞显示选区
半透明遮罩 + 挖洞 + 尺寸标注
"""

from PyQt6.QtCore import QRectF, Qt, QPointF
from PyQt6.QtGui import QPainterPath, QColor, QBrush, QPen, QPainter, QFont
from PyQt6.QtWidgets import QGraphicsPathItem, QStyleOptionGraphicsItem, QWidget
from canvas.model import SelectionModel


class OverlayMaskItem(QGraphicsPathItem):
    """
    遮罩层 - 半透明遮罩 + 挖洞显示选区 + 尺寸标注
    Z-order: 10
    """
    
    def __init__(self, full_rect: QRectF, model: SelectionModel):
        super().__init__()
        self.setZValue(10)
        
        self._full = QRectF(full_rect)
        self._model = model
        
        # 连接选区变化信号
        self._model.rectChanged.connect(self.rebuild)
        
        # 初始构建
        self.rebuild()
        
        # 确保可见
        self.setVisible(True)
        self.setEnabled(True)
        
        print(f"✅ [遮罩层] 创建: {full_rect}, Z-order: {self.zValue()}, 可见: {self.isVisible()}")
    
    def rebuild(self, *_):
        """重建遮罩路径（挖洞）"""
        # 全屏路径
        all_path = QPainterPath()
        all_path.addRect(self._full)
        
        # 选区路径（洞）
        hole_path = QPainterPath()
        if not self._model.is_empty():
            selection_rect = self._model.rect()
            hole_path.addRect(selection_rect)
        
        # 挖洞: 全屏 - 选区
        final_path = all_path.subtracted(hole_path)
        self.setPath(final_path)
        
        # 设置样式
        self.setBrush(QBrush(QColor(0, 0, 0, 120)))  # 半透明黑色（与老代码一致）
        self.setPen(QPen(Qt.PenStyle.NoPen))  # 无边框
        
        # 触发重绘（用于绘制尺寸标注）
        self.update()
    
    def paint(self, painter: QPainter, option: QStyleOptionGraphicsItem, widget: QWidget = None):
        """绘制遮罩和尺寸标注"""
        # 先绘制遮罩
        super().paint(painter, option, widget)
        
        # 如果有选区，绘制尺寸标注
        if not self._model.is_empty():
            rect = self._model.rect()
            width = int(rect.width())
            height = int(rect.height())
            
            # 计算标注位置（左上角内侧，偏移5像素）
            text_x = rect.left() + 5
            text_y = rect.top() + 18  # 字体大小约14，所以偏移18
            
            # 如果选区太靠边，调整位置
            if rect.left() < 5:
                text_x = rect.left() + rect.width() - 72
            if rect.top() < 20:
                text_y = rect.top() + rect.height() - 5
            
            # 绘制尺寸文字
            painter.setFont(QFont("Arial", 12))
            painter.setPen(QPen(QColor(32, 178, 170), 2, Qt.PenStyle.SolidLine))  # 青绿色
            painter.drawText(QPointF(text_x, text_y), f"{width}x{height}")

