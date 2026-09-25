"""光标旁的一行轻提示：不抢焦点、不接鼠标，停一会儿自己淡出

给跳过结果窗口的快捷行为报结果用。用户接下来要回原程序粘贴，提示不能把焦点拿走。
"""

from PySide6.QtCore import QPropertyAnimation, Qt, QTimer
from PySide6.QtGui import QCursor
from PySide6.QtWidgets import QApplication, QLabel, QVBoxLayout, QWidget

from core.ui_scale import dialog_scaled
from ui.dialogs import track_modeless_dialog
from ui.fluent_lite import FONT_FAMILY, ui_tokens


class Toast(QWidget):
    """构造即显示在光标旁；finish() 之后停留 HOLD_MS 再淡出销毁。

    要等结果的场景（识别中…）先构造、结果回来再 finish；一次性提示用 show_toast。
    """

    HOLD_MS = 1500
    FADE_MS = 200
    MAX_TEXT_WIDTH = 360

    def __init__(self, text):
        super().__init__(None)
        self.setWindowFlags(
            Qt.WindowType.ToolTip
            | Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.WindowStaysOnTopHint
            | Qt.WindowType.WindowTransparentForInput
        )
        self.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating, True)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, True)
        self.setAttribute(Qt.WidgetAttribute.WA_DeleteOnClose, True)

        t = ui_tokens()
        self.label = QLabel(self)
        self.label.setTextFormat(Qt.TextFormat.PlainText)
        self.label.setStyleSheet(
            f"QLabel {{ background: {t.popup_background}; color: {t.text}; "
            f"border: 1px solid {t.window_border}; border-radius: {dialog_scaled(8)}px; "
            f"padding: {dialog_scaled(6)}px {dialog_scaled(12)}px; "
            f"font: {dialog_scaled(13)}px {FONT_FAMILY}; }}"
        )
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(self.label)

        # 位置跟着出现那一刻的光标走，之后换文案也不追着鼠标跑
        self._anchor = QCursor.pos()
        self._finished = False
        self.set_text(text)
        track_modeless_dialog(self)
        self.show()

    def set_text(self, text):
        """换行压成空格、过长截断：提示只占一行"""
        self.label.ensurePolished()
        line = " ".join(text.split())
        self.label.setText(self.label.fontMetrics().elidedText(
            line, Qt.TextElideMode.ElideRight, dialog_scaled(self.MAX_TEXT_WIDTH)))
        self.adjustSize()
        self._place()

    def finish(self, text=None):
        if text is not None:
            self.set_text(text)
        if self._finished:
            return
        self._finished = True
        QTimer.singleShot(self.HOLD_MS, self, self._fade_out)

    def _fade_out(self):
        fade = QPropertyAnimation(self, b"windowOpacity", self)
        fade.setDuration(self.FADE_MS)
        fade.setStartValue(1.0)
        fade.setEndValue(0.0)
        fade.finished.connect(self.close)
        fade.start()

    def _place(self):
        """默认在光标右下，贴到屏幕底边就翻到光标上方"""
        screen = QApplication.screenAt(self._anchor) or QApplication.primaryScreen()
        area = screen.availableGeometry()
        offset = dialog_scaled(16)
        x = min(self._anchor.x() + offset, area.right() - self.width())
        y = self._anchor.y() + offset
        if y + self.height() > area.bottom():
            y = self._anchor.y() - offset - self.height()
        self.move(max(x, area.left()), max(y, area.top()))


def show_toast(text):
    toast = Toast(text)
    toast.finish()
    return toast
