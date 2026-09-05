"""Native Qt buttons styled as a cohesive modern Fluent-like family."""

from __future__ import annotations

from PySide6.QtCore import QSize, QUrl
from PySide6.QtGui import QDesktopServices
from PySide6.QtWidgets import QPushButton, QToolButton, QWidget

from core.ui_theme import get_ui_theme

from .theme import (
    ACCENT, ACCENT_HOVER, ACCENT_PRESSED, FONT_FAMILY, to_qicon, ui_tokens,
)


def _base_style(widget=None) -> str:
    t = ui_tokens(widget)
    return f"""
    QPushButton {{ min-height: 28px; padding: 4px 16px; color: {t.text};
     background: {t.surface}; border: 1px solid {t.border}; border-radius: 10px;
     font: 13px {FONT_FAMILY}; outline: none; }}
    QPushButton:hover {{ background: {t.surface_strong}; border-color: {t.border_hover}; }}
    QPushButton:pressed {{ background: {t.surface_subtle}; padding-top: 5px; padding-bottom: 3px; }}
    QPushButton:focus {{ border-color: {ACCENT}; }}
    QPushButton:disabled {{ color: {t.text_disabled}; background: {t.surface_subtle}; border-color: {t.border}; }}
    """


def _primary_style() -> str:
    return f"""
    QPushButton {{ min-height: 28px; padding: 4px 18px; color: white;
     background: {ACCENT}; border: 1px solid rgba(255,255,255,.34); border-radius: 10px;
     font: 600 13px {FONT_FAMILY}; outline: none; }}
    QPushButton:hover {{ background: {ACCENT_HOVER}; border-color: {ACCENT_HOVER}; }}
    QPushButton:pressed {{ background: {ACCENT_PRESSED}; border-color: {ACCENT_PRESSED}; padding-top: 5px; padding-bottom: 3px; }}
    QPushButton:disabled {{ color: rgba(255,255,255,.84); background: #687D8F; border-color: #687D8F; }}
    """


def _transparent_style(widget=None) -> str:
    t = ui_tokens(widget)
    return f"""
    QPushButton {{ min-height: 28px; padding: 4px 12px; color: {t.text_muted};
     background: transparent; border: 1px solid transparent; border-radius: 10px;
     font: 13px {FONT_FAMILY}; outline: none; }}
    QPushButton:hover {{ color: {t.text}; background: {t.surface_subtle}; }}
    QPushButton:pressed {{ background: {t.surface}; }}
    QPushButton:disabled {{ color: {t.text_disabled}; }}
    """


def _button_args(text, parent, icon):
    if isinstance(text, QWidget):
        parent, text = text, ""
    elif not isinstance(text, str) and text is not None:
        icon, text = text, ""
    return text or "", parent, icon


class PushButton(QPushButton):
    def __init__(self, text="", parent=None, icon=None):
        text, parent, icon = _button_args(text, parent, icon)
        super().__init__(text, parent)
        self._theme_icon_source = None
        self.setCursor(self.cursor().shape())
        self._apply_theme()
        get_ui_theme().theme_changed.connect(self._apply_theme)
        self.setIconSize(QSize(16, 16))
        if icon is not None:
            self.setIcon(icon)

    def setIcon(self, icon):
        self._theme_icon_source = icon
        super().setIcon(to_qicon(icon, self))

    def _style_sheet(self):
        return _base_style(self)

    def _apply_theme(self, _tokens=None):
        self.setStyleSheet(self._style_sheet())
        if self._theme_icon_source is not None:
            super().setIcon(to_qicon(self._theme_icon_source, self))


class PrimaryPushButton(PushButton):
    def _style_sheet(self):
        return _primary_style()


class TransparentPushButton(PushButton):
    def _style_sheet(self):
        return _transparent_style(self)


class HyperlinkButton(TransparentPushButton):
    def __init__(self, url="", text="", parent=None, icon=None):
        super().__init__(text, parent, icon)
        self._url = url
        self.clicked.connect(self._open_url)

    def _style_sheet(self):
        return _transparent_style(self) + f"QPushButton {{ color: {ACCENT}; }}"

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
        self._theme_icon_source = None
        self.setFixedSize(30, 30)
        self.setIconSize(QSize(16, 16))
        self._apply_theme()
        get_ui_theme().theme_changed.connect(self._apply_theme)
        if icon is not None:
            self.setIcon(icon)

    def setIcon(self, icon):
        self._theme_icon_source = icon
        super().setIcon(to_qicon(icon, self))

    def _apply_theme(self, _tokens=None):
        t = ui_tokens(self)
        self.setStyleSheet(f"""
            QToolButton {{ background: transparent; border: none; border-radius: 9px; }}
            QToolButton:hover {{ background: {t.surface_hover}; }}
            QToolButton:pressed {{ background: {t.surface_subtle}; }}
        """)
        if self._theme_icon_source is not None:
            super().setIcon(to_qicon(self._theme_icon_source, self))
