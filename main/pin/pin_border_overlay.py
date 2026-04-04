"""
钉图主题色描边 Overlay

轻量方案：只画一圈 1-2px 圆角描边，无阴影。
每帧 paintEvent 仅调用一次 drawRoundedRect，性能消耗极低（≈ 5-15 μs）。
"""

from PySide6.QtWidgets import QWidget
from PySide6.QtCore import Qt, QRectF
from PySide6.QtGui import QPainter, QPen, QColor
from core import safe_event


class PinBorderOverlay(QWidget):

    def __init__(self, parent=None, corner_radius=2, border_color: QColor = None):
        super().__init__(parent)

        self.corner_radius = corner_radius
        self.border_color = border_color or QColor(88, 94, 184, 200)

        # 透明背景，鼠标穿透，不接收焦点
        self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.setAutoFillBackground(False)
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)

    # ------------------------------------------------------------------
    # Qt 事件
    # ------------------------------------------------------------------

    @safe_event
    def paintEvent(self, event):
        """单圈主题色描边"""
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        pen = QPen(self.border_color, 3)
        pen.setCosmetic(True)
        painter.setPen(pen)
        painter.setBrush(Qt.BrushStyle.NoBrush)
        r = QRectF(self.rect()).adjusted(1.5, 1.5, -1.5, -1.5)
        painter.drawRoundedRect(r, self.corner_radius, self.corner_radius)
        painter.end()

    # ------------------------------------------------------------------
    # 公开接口
    # ------------------------------------------------------------------

    def set_corner_radius(self, radius: float):
        self.corner_radius = radius
        self.update()

    def set_border_color(self, color: QColor):
        self.border_color = color
        self.update()
 