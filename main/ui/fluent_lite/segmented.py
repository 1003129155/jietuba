"""Compact segmented selector with route-key compatibility."""

from PySide6.QtCore import Signal
from PySide6.QtWidgets import QButtonGroup, QHBoxLayout, QPushButton, QWidget

from .theme import ACCENT, FONT_FAMILY, TEXT, TEXT_MUTED


class SegmentedWidget(QWidget):
    currentItemChanged = Signal(str)

    def __init__(self, parent=None):
        super().__init__(parent)
        self._items = {}
        self._current = None
        self._indicator = ACCENT
        self._group = QButtonGroup(self)
        self._group.setExclusive(True)
        self._layout = QHBoxLayout(self)
        self._layout.setContentsMargins(3, 3, 3, 3)
        self._layout.setSpacing(3)
        self.setStyleSheet("background: rgba(225,232,227,.70); border: 1px solid rgba(255,255,255,.70); border-radius: 11px;")

    def _style(self):
        return f"""
            QPushButton {{ padding: 4px 14px; color: {TEXT_MUTED}; background: transparent;
                border: none; border-radius: 9px; font: 12px {FONT_FAMILY}; }}
            QPushButton:hover {{ color: {TEXT}; background: rgba(255,255,255,.46); }}
            QPushButton:checked {{ color: {TEXT}; background: rgba(255,255,255,.94); border: 1px solid rgba(255,255,255,.98);
                font-weight: 600; }}
        """

    def addItem(self, routeKey, text, onClick=None):
        self.insertItem(len(self._items), routeKey, text, onClick)

    def insertItem(self, index, routeKey, text, onClick=None):
        button = QPushButton(str(text), self)
        button.setCheckable(True)
        button.setStyleSheet(self._style())
        button.clicked.connect(lambda checked=False, key=routeKey: self._on_clicked(key))
        if onClick:
            button.clicked.connect(onClick)
        self._group.addButton(button)
        self._layout.insertWidget(index, button)
        self._items[routeKey] = button

    def setCurrentItem(self, routeKey):
        if routeKey not in self._items:
            return
        self._items[routeKey].setChecked(True)
        if self._current != routeKey:
            self._current = routeKey
            self.currentItemChanged.emit(routeKey)

    def currentItem(self):
        return self._current

    def setIndicatorColor(self, light, dark=None):
        self._indicator = str(light)

    def _on_clicked(self, routeKey):
        self.setCurrentItem(routeKey)
