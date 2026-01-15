"""
Mac 风格阴影 Overlay Widget - 静态 UI 装饰层

"""

from PyQt6.QtWidgets import QWidget
from PyQt6.QtCore import Qt
from PyQt6.QtGui import QPainter, QPen, QColor


class PinBorderOverlay(QWidget):

    
    def __init__(self, parent=None, corner_radius=16):
        super().__init__(parent)
        
        self.corner_radius = corner_radius
        
        # Mac 风格多层阴影配置 (offset, alpha)
        # 从内到外，逐渐扩散
        self.shadow_layers = [
            (1, 200),
            (2, 150),
            (3, 80),
            (4, 60),
            (5, 40),
            (6, 30),
            (7, 20),
            (8, 10),
            (9, 5),
        ]
        
        # 设置为透明背景，鼠标穿透
        self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.setAutoFillBackground(False)
        
        # 不接收焦点
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)
    
    def paintEvent(self, event):
        """绘制 Mac 风格内阴影"""
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        
        base_rect = self.rect()
        
        # 绘制多层阴影（从外到内）
        for offset, alpha in self.shadow_layers:
            pen = QPen(QColor(0, 0, 0, alpha), 1)
            painter.setPen(pen)
            painter.setBrush(Qt.BrushStyle.NoBrush)
            
            # 向内收缩，创建内阴影效果
            rect = base_rect.adjusted(
                offset, offset, -offset, -offset
            )
            painter.drawRoundedRect(rect, self.corner_radius, self.corner_radius)
        
        painter.end()
    
    def set_corner_radius(self, radius):
        """设置圆角半径"""
        self.corner_radius = radius
        self.update()
    
    def set_shadow_layers(self, layers):
        """设置阴影层配置 [(offset, alpha), ...]"""
        self.shadow_layers = layers
        self.update()
