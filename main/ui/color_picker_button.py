"""可复用的颜色选择按钮组件"""
from PySide6.QtWidgets import QPushButton, QColorDialog, QApplication
from PySide6.QtCore import Qt, Signal, QPoint, QRectF
from PySide6.QtGui import QColor, QPainter, QPen, QBrush, QConicalGradient


class ColorPickerButton(QPushButton):
    """颜色选择按钮

    点击后弹出 QColorDialog，颜色改变时发射 color_changed 信号
    并自动更新按钮的显示颜色。

    Args:
        initial_color: 初始颜色
        show_alpha:    是否在对话框中显示透明度通道（默认 True）
        size:          按钮边长（像素，正方形）
        rainbow_ring:  是否在按钮外圈绘制彩虹色描边，标识"自定义颜色"入口（默认 True）
        parent:        父控件
    """

    color_changed = Signal(QColor)

    # 彩虹描边的色相分布（红→黄→绿→青→蓝→品红→回到红）
    _RING_STOPS = (
        (0.0, 0), (1 / 6, 60), (2 / 6, 120), (3 / 6, 180),
        (4 / 6, 240), (5 / 6, 300), (1.0, 360),
    )

    def __init__(
        self,
        initial_color: QColor,
        *,
        show_alpha: bool = True,
        size: int = 28,
        rainbow_ring: bool = True,
        parent=None,
    ):
        super().__init__(parent)
        self._color = QColor(initial_color)
        self._show_alpha = show_alpha
        self._rainbow_ring = rainbow_ring
        self.setFixedSize(size, size)
        if self._rainbow_ring:
            self.setFlat(True)
            self.setAttribute(Qt.WidgetAttribute.WA_Hover, True)
            self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, True)
            self.setCursor(Qt.CursorShape.PointingHandCursor)
            self.setStyleSheet("QPushButton { border: none; background: transparent; }")
        else:
            self._update_style()
        self.clicked.connect(self._pick_color)

    @property
    def color(self) -> QColor:
        return QColor(self._color)

    def set_color(self, color: QColor) -> None:
        """静默更新颜色（不发射 color_changed 信号）"""
        self._color = QColor(color)
        self._refresh()

    def _pick_color(self) -> None:
        dlg = QColorDialog(self._color, None)
        dlg.setWindowFlag(Qt.WindowType.WindowStaysOnTopHint, True)
        if self._show_alpha:
            dlg.setOption(QColorDialog.ColorDialogOption.ShowAlphaChannel)

        # 定位到按钮下方附近，超出屏幕则自动调整
        btn_global = self.mapToGlobal(QPoint(0, self.height() + 4))
        dlg.adjustSize()
        screen = QApplication.screenAt(btn_global) or QApplication.primaryScreen()
        sg = screen.availableGeometry()
        x = btn_global.x()
        y = btn_global.y()
        if x + dlg.width() > sg.right():
            x = sg.right() - dlg.width()
        if y + dlg.height() > sg.bottom():
            y = self.mapToGlobal(QPoint(0, 0)).y() - dlg.height() - 4
        x = max(sg.left(), x)
        y = max(sg.top(), y)
        dlg.move(x, y)

        if dlg.exec():
            self._color = dlg.selectedColor()
            self._refresh()
            self.color_changed.emit(QColor(self._color))

    def _refresh(self) -> None:
        if self._rainbow_ring:
            self.update()
        else:
            self._update_style()

    def enterEvent(self, event) -> None:
        self.update()
        super().enterEvent(event)

    def leaveEvent(self, event) -> None:
        self.update()
        super().leaveEvent(event)

    def paintEvent(self, event) -> None:
        if not self._rainbow_ring:
            super().paintEvent(event)
            return

        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)

        size = min(self.width(), self.height())
        ring_width = max(1.8, size * 0.11)
        gap = max(1.0, size * 0.05)

        outer_rect = QRectF(self.rect()).adjusted(
            ring_width / 2, ring_width / 2, -ring_width / 2, -ring_width / 2
        )
        outer_radius = size * 0.26

        # 外框：沿用原本的圆角方形，只是把描边换成彩虹色，标识"自定义颜色"入口
        gradient = QConicalGradient(outer_rect.center(), 90)
        for stop, hue in self._RING_STOPS:
            gradient.setColorAt(stop, QColor.fromHsv(hue % 360, 255, 255))
        painter.setPen(QPen(QBrush(gradient), ring_width))
        painter.setBrush(Qt.BrushStyle.NoBrush)
        painter.drawRoundedRect(outer_rect, outer_radius, outer_radius)

        # 内部：当前选中的颜色
        inset = ring_width / 2 + gap
        inner_rect = outer_rect.adjusted(inset, inset, -inset, -inset)
        inner_radius = max(1.0, outer_radius - inset)
        if self.isDown():
            shrink = inner_rect.width() * 0.06
            inner_rect = inner_rect.adjusted(shrink, shrink, -shrink, -shrink)
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(QBrush(self._color))
        painter.drawRoundedRect(inner_rect, inner_radius, inner_radius)

        # 悬停高光
        if self.underMouse() and not self.isDown():
            painter.setPen(QPen(QColor(0, 0, 0, 70), 1.2))
            painter.setBrush(Qt.BrushStyle.NoBrush)
            painter.drawRoundedRect(inner_rect, inner_radius, inner_radius)

    def _update_style(self) -> None:
        """旧版方形样式（rainbow_ring=False 时使用）"""
        color_hex = self._color.name()
        border_color = "#888888" if self._color.lightness() > 200 else "#333333"
        self.setStyleSheet(f"""
            QPushButton {{
                background-color: {color_hex};
                border: 2px solid {border_color};
                border-radius: 3px;
            }}
            QPushButton:hover {{
                background-color: {color_hex};
                border: 2px solid #000;
            }}
            QPushButton:pressed {{
                background-color: {color_hex};
                border: 3px solid #000;
            }}
        """)
