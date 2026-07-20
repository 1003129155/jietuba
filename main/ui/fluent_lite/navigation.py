"""Lightweight route-based sidebar navigation."""

from enum import Enum

from PySide6.QtCore import QSize, Qt
from PySide6.QtWidgets import QButtonGroup, QPushButton, QVBoxLayout, QWidget

from core.ui_theme import get_ui_theme

from .theme import ACCENT, FONT_FAMILY, to_qicon, ui_tokens


class NavigationItemPosition(Enum):
    TOP = 0
    BOTTOM = 1


class NavigationInterface(QWidget):
    def __init__(self, parent=None, showMenuButton=False, showReturnButton=False, collapsible=False):
        super().__init__(parent)
        self._items = {}
        self._icons = {}
        self._current = None
        self._group = QButtonGroup(self)
        self._group.setExclusive(True)
        self._layout = QVBoxLayout(self)
        self._layout.setContentsMargins(0, 0, 0, 0)
        self._layout.setSpacing(5)
        self._top = QVBoxLayout()
        self._top.setSpacing(5)
        self._bottom = QVBoxLayout()
        self._bottom.setSpacing(5)
        self._layout.addLayout(self._top)
        self._layout.addStretch(1)
        self._layout.addLayout(self._bottom)
        self.setStyleSheet("background: transparent;")
        get_ui_theme().theme_changed.connect(self._apply_theme)

    def addItem(self, routeKey, icon, text, onClick, position=NavigationItemPosition.TOP, tooltip=None):
        button = QPushButton(str(text), self)
        button.setObjectName("FluentLiteNavItem")
        button.setCheckable(True)
        button.setIcon(to_qicon(icon, button))
        button.setIconSize(QSize(18, 18))
        button.setCursor(Qt.CursorShape.PointingHandCursor)
        # Labels are already visible in the expanded navigation.  Creating a
        # duplicate tooltip for every item leaves an unwanted popup on hover;
        # only opt in when a caller has genuinely extra context to show.
        if tooltip:
            button.setToolTip(str(tooltip))
        button.setStyleSheet(f"""
            QPushButton#FluentLiteNavItem {{ min-height: 40px; padding: 2px 13px; text-align: left;
                color: #000000; background: transparent; border: 1px solid transparent; border-radius: 11px;
                font: 13px {FONT_FAMILY}; }}
            QPushButton#FluentLiteNavItem:hover {{ color: #000000; background: rgba(255,255,255,.46); }}
            QPushButton#FluentLiteNavItem:checked {{ color: #000000; background: rgba(255,255,255,.82);
                border: 1px solid rgba(255,255,255,.94); border-left: 4px solid {ACCENT};
                padding-left: 10px; font-weight: 600; }}
        """)
        button.clicked.connect(lambda checked=False, key=routeKey: self.setCurrentItem(key))
        if onClick:
            button.clicked.connect(onClick)
        self._group.addButton(button)
        self._items[routeKey] = button
        self._icons[routeKey] = icon
        self._style_button(button)
        target = self._bottom if position == NavigationItemPosition.BOTTOM else self._top
        target.addWidget(button)
        return button

    def _style_button(self, button):
        t = ui_tokens(button)
        button.setStyleSheet(f"""
            QPushButton#FluentLiteNavItem {{ min-height: 40px; padding: 2px 13px; text-align: left;
                color: {t.text}; background: transparent; border: 1px solid transparent; border-radius: 11px;
                font: 13px {FONT_FAMILY}; }}
            QPushButton#FluentLiteNavItem:hover {{ color: {t.text}; background: {t.surface_subtle}; }}
            QPushButton#FluentLiteNavItem:checked {{ color: {t.text}; background: {t.surface_strong};
                border: 1px solid {t.border}; border-left: 4px solid {ACCENT};
                padding-left: 10px; font-weight: 600; }}
        """)

    def _apply_theme(self, _tokens=None):
        for route_key, button in self._items.items():
            button.setIcon(to_qicon(self._icons[route_key], button))
            self._style_button(button)

    def setCurrentItem(self, routeKey):
        button = self._items.get(routeKey)
        if button is None:
            return
        self._current = routeKey
        button.setChecked(True)

    def clearCurrentItem(self):
        self._group.setExclusive(False)
        for button in self._items.values():
            button.setChecked(False)
        self._group.setExclusive(True)
        self._current = None

    def setExpandWidth(self, width):
        self._expand_width = int(width)

    def setMinimumExpandWidth(self, width):
        self._minimum_expand_width = int(width)

    def expand(self, useAni=True):
        if getattr(self, "_expand_width", 0):
            self.setMinimumWidth(self._expand_width)

    def widget(self, routeKey):
        return self._items.get(routeKey)
