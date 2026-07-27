"""Minimal title bar compatible with qframelesswindow.FramelessDialog."""

from PySide6.QtCore import Qt
from PySide6.QtGui import QColor
from PySide6.QtWidgets import QLabel
from qframelesswindow import TitleBar

from core.ui_theme import get_ui_theme

from .theme import FONT_FAMILY, ui_tokens


class FluentTitleBar(TitleBar):
    def __init__(self, parent):
        super().__init__(parent)
        self.setFixedHeight(32)
        self.buttonLayout = self.hBoxLayout
        self.iconLabel = QLabel(self)
        self.iconLabel.setFixedSize(22, 22)
        self.iconLabel.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.titleLabel = QLabel(parent.windowTitle(), self)
        self._apply_theme()
        get_ui_theme().theme_changed.connect(self._apply_theme)
        self.hBoxLayout.insertWidget(0, self.iconLabel)
        self.hBoxLayout.insertWidget(1, self.titleLabel)
        parent.windowTitleChanged.connect(self.setTitle)
        parent.windowIconChanged.connect(self.setIcon)
        self.setStyleSheet("background: transparent;")

    def _apply_theme(self, _tokens=None):
        tokens = ui_tokens(self)
        self.titleLabel.setStyleSheet(
            f"color: {tokens.text}; font: 12px {FONT_FAMILY}; "
            "background: transparent;"
        )
        for button in (self.minBtn, self.maxBtn, self.closeBtn):
            button.setNormalColor(QColor(tokens.text_muted))
            button.setHoverColor(QColor(tokens.text))
            button.setPressedColor(QColor(tokens.text))
            button.setNormalBackgroundColor(QColor(0, 0, 0, 0))
            button.setHoverBackgroundColor(QColor(tokens.surface_hover))
            button.setPressedBackgroundColor(QColor(tokens.accent_soft))
        self.closeBtn.setHoverColor(QColor("#FFFFFF"))
        self.closeBtn.setHoverBackgroundColor(QColor("#C42B1C"))

    def setTitle(self, title):
        self.titleLabel.setText(str(title))

    def setIcon(self, icon):
        self.iconLabel.setPixmap(icon.pixmap(16, 16))
