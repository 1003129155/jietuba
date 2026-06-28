"""Native Qt buttons styled as a cohesive modern Fluent-like family."""

from __future__ import annotations

from PySide6.QtCore import QSize, QUrl
from PySide6.QtGui import QDesktopServices, QIcon
from PySide6.QtWidgets import QPushButton, QToolButton, QWidget

from .theme import (
    ACCENT, ACCENT_HOVER, ACCENT_PRESSED, BORDER, BORDER_HOVER,
    FONT_FAMILY, SURFACE, SURFACE_HOVER, SURFACE_STRONG, TEXT, TEXT_MUTED, to_qicon,
)


_BASE = f"""
QPushButton {{ min-height: 28px; padding: 4px 16px; color: {TEXT};
 background: {SURFACE}; border: 1px solid {BORDER}; border-radius: 10px;
 font: 13px {FONT_FAMILY}; outline: none; }}
QPushButton:hover {{ background: {SURFACE_STRONG}; border-color: {BORDER_HOVER}; }}
QPushButton:pressed {{ background: rgba(226, 237, 230, 0.92); padding-top: 5px; padding-bottom: 3px; }}
QPushButton:focus {{ border-color: {ACCENT}; }}
QPushButton:disabled {{ color: #A2AAA4; background: #F4F6F4; border-color: #E5E9E6; }}
"""
_PRIMARY = f"""
QPushButton {{ min-height: 28px; padding: 4px 18px; color: white;
 background: {ACCENT}; border: 1px solid rgba(255,255,255,.34); border-radius: 10px;
 font: 600 13px {FONT_FAMILY}; outline: none; }}
QPushButton:hover {{ background: {ACCENT_HOVER}; border-color: {ACCENT_HOVER}; }}
QPushButton:pressed {{ background: {ACCENT_PRESSED}; border-color: {ACCENT_PRESSED}; padding-top: 5px; padding-bottom: 3px; }}
QPushButton:disabled {{ color: rgba(255,255,255,.84); background: #A9CBEA; border-color: #A9CBEA; }}
"""
_TRANSPARENT = f"""
QPushButton {{ min-height: 28px; padding: 4px 12px; color: {TEXT_MUTED};
 background: transparent; border: 1px solid transparent; border-radius: 10px;
 font: 13px {FONT_FAMILY}; outline: none; }}
QPushButton:hover {{ color: {TEXT}; background: rgba(24,33,27,.055); }}
QPushButton:pressed {{ background: rgba(24,33,27,.09); }}
QPushButton:disabled {{ color: #ADB4AF; }}
"""


def _button_args(text, parent, icon):
    if isinstance(text, QWidget):
        parent, text = text, ""
    elif not isinstance(text, str) and text is not None:
        icon, text = text, ""
    return text or "", parent, icon


class PushButton(QPushButton):
    _qss = _BASE

    def __init__(self, text="", parent=None, icon=None):
        text, parent, icon = _button_args(text, parent, icon)
        super().__init__(text, parent)
        self.setCursor(self.cursor().shape())
        self.setStyleSheet(self._qss)
        self.setIconSize(QSize(16, 16))
        if icon is not None:
            self.setIcon(icon)

    def setIcon(self, icon):
        super().setIcon(to_qicon(icon))


class PrimaryPushButton(PushButton):
    _qss = _PRIMARY


class TransparentPushButton(PushButton):
    _qss = _TRANSPARENT


class HyperlinkButton(TransparentPushButton):
    def __init__(self, url="", text="", parent=None, icon=None):
        super().__init__(text, parent, icon)
        self._url = url
        self.setStyleSheet(_TRANSPARENT + f"QPushButton {{ color: {ACCENT}; }}")
        self.clicked.connect(self._open_url)

    def _open_url(self):
        if self._url:
            QDesktopServices.openUrl(QUrl(self._url))

    def setUrl(self, url):
        self._url = str(url)


class TransparentToolButton(QToolButton):
    def __init__(self, icon=None, parent=None):
        if isinstance(icon, QWidget):
            parent, icon = icon, None
        super().__init__(parent)
        self.setFixedSize(30, 30)
        self.setIconSize(QSize(16, 16))
        self.setStyleSheet(f"""
            QToolButton {{ background: transparent; border: none; border-radius: 9px; }}
            QToolButton:hover {{ background: {SURFACE_HOVER}; }}
            QToolButton:pressed {{ background: rgba(208, 222, 235, .72); }}
        """)
        if icon is not None:
            self.setIcon(icon)

    def setIcon(self, icon):
        super().setIcon(to_qicon(icon))
