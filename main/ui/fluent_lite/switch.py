"""Animated accessible switch control implemented with pure PySide6."""

from PySide6.QtCore import Property, QEasingCurve, QPropertyAnimation, QRectF, Qt, Signal, QSize
from PySide6.QtGui import QColor, QPainter
from PySide6.QtWidgets import QWidget

from .theme import ACCENT


class SwitchButton(QWidget):
    checkedChanged = Signal(bool)

    def __init__(self, parent=None, indicatorPos=None):
        super().__init__(parent)
        self._checked = False
        self._on_text = ""
        self._off_text = ""
        self._progress = 0.0
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self._animation = QPropertyAnimation(self, b"progress", self)
        self._animation.setDuration(145)
        self._animation.setEasingCurve(QEasingCurve.Type.OutCubic)
        self.setFixedSize(46, 26)

    def sizeHint(self):
        return QSize(46, 26)

    def _get_progress(self):
        return self._progress

    def _set_progress(self, value):
        self._progress = float(value)
        self.update()

    progress = Property(float, _get_progress, _set_progress)

    def isChecked(self):
        return self._checked

    def setChecked(self, checked):
        checked = bool(checked)
        if self._checked == checked:
            self._progress = 1.0 if checked else 0.0
            self.update()
            return
        self._checked = checked
        self._animate()
        self.checkedChanged.emit(checked)

    def setOnText(self, text):
        self._on_text = str(text)

    def setOffText(self, text):
        self._off_text = str(text)

    def _animate(self):
        self._animation.stop()
        self._animation.setStartValue(self._progress)
        self._animation.setEndValue(1.0 if self._checked else 0.0)
        self._animation.start()

    def mouseReleaseEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton and self.rect().contains(event.position().toPoint()):
            self.setChecked(not self._checked)
            event.accept()
            return
        super().mouseReleaseEvent(event)

    def keyPressEvent(self, event):
        if event.key() in (Qt.Key.Key_Space, Qt.Key.Key_Return, Qt.Key.Key_Enter):
            self.setChecked(not self._checked)
            event.accept()
            return
        super().keyPressEvent(event)

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        track = QRectF(0.5, 0.5, self.width() - 1, self.height() - 1)
        off = QColor("#C4CCC6")
        on = QColor(ACCENT)
        r = int(off.red() + (on.red() - off.red()) * self._progress)
        g = int(off.green() + (on.green() - off.green()) * self._progress)
        b = int(off.blue() + (on.blue() - off.blue()) * self._progress)
        if not self.isEnabled():
            r, g, b = 205, 211, 207
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(QColor(r, g, b))
        painter.drawRoundedRect(track, 13, 13)
        knob = 20.0
        x = 3.0 + (self.width() - knob - 6.0) * self._progress
        painter.setBrush(QColor("#FFFFFF"))
        painter.drawEllipse(QRectF(x, 3.0, knob, knob))
