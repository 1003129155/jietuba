"""可复用的颜色选择按钮组件"""
from PySide6.QtWidgets import QPushButton, QColorDialog, QApplication
from PySide6.QtCore import Qt, Signal, QPoint
from PySide6.QtGui import QColor


class ColorPickerButton(QPushButton):
    """颜色选择按钮

    点击后弹出 QColorDialog，颜色改变时发射 color_changed 信号
    并自动更新按钮的背景色样式。

    Args:
        initial_color: 初始颜色
        show_alpha:    是否在对话框中显示透明度通道（默认 True）
        size:          按钮边长（像素，正方形）
        parent:        父控件
    """

    color_changed = Signal(QColor)

    def __init__(
        self,
        initial_color: QColor,
        *,
        show_alpha: bool = True,
        size: int = 28,
        parent=None,
    ):
        super().__init__(parent)
        self._color = QColor(initial_color)
        self._show_alpha = show_alpha
        self.setFixedSize(size, size)
        self._update_style()
        self.clicked.connect(self._pick_color)

    @property
    def color(self) -> QColor:
        return QColor(self._color)

    def set_color(self, color: QColor) -> None:
        """静默更新颜色（不发射 color_changed 信号）"""
        self._color = QColor(color)
        self._update_style()

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
            self._update_style()
            self.color_changed.emit(QColor(self._color))

    def _update_style(self) -> None:
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
 