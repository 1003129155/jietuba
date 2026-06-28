"""Minimal title bar compatible with qframelesswindow.FramelessDialog."""

from PySide6.QtCore import Qt
from PySide6.QtWidgets import QLabel
from qframelesswindow import TitleBar

from .theme import FONT_FAMILY, TEXT


class FluentTitleBar(TitleBar):
    def __init__(self, parent):
        super().__init__(parent)
        self.setFixedHeight(32)
        self.buttonLayout = self.hBoxLayout
        self.iconLabel = QLabel(self)
        self.iconLabel.setFixedSize(22, 22)
        self.iconLabel.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.titleLabel = QLabel(parent.windowTitle(), self)
        self.titleLabel.setStyleSheet(f"color: {TEXT}; font: 12px {FONT_FAMILY}; background: transparent;")
        self.hBoxLayout.insertWidget(0, self.iconLabel)
        self.hBoxLayout.insertWidget(1, self.titleLabel)
        parent.windowTitleChanged.connect(self.setTitle)
        parent.windowIconChanged.connect(self.setIcon)
        self.setStyleSheet("background: transparent;")

    def setTitle(self, title):
        self.titleLabel.setText(str(title))

    def setIcon(self, icon):
        self.iconLabel.setPixmap(icon.pixmap(16, 16))
