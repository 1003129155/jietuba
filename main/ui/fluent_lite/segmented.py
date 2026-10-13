"""Compact segmented selector with route-key compatibility."""

from PySide6.QtCore import Signal
from PySide6.QtGui import QFont, QFontMetrics
from PySide6.QtWidgets import QButtonGroup, QHBoxLayout, QPushButton, QWidget

from core.ui_scale import dialog_scaled
from core.ui_theme import apply_style_sheet, get_ui_theme, set_own_style

from .theme import ACCENT, FONT_FAMILIES, FONT_FAMILY, ui_tokens


def _bold_text_width(text: str) -> int:
    font = QFont()
    font.setFamilies(FONT_FAMILIES)
    font.setPixelSize(dialog_scaled(12))
    font.setWeight(QFont.Weight.DemiBold)
    return QFontMetrics(font).horizontalAdvance(text)


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
        self._layout.setContentsMargins(
            dialog_scaled(3), dialog_scaled(3), dialog_scaled(3), dialog_scaled(3)
        )
        self._layout.setSpacing(dialog_scaled(3))
        self._apply_theme()
        get_ui_theme().theme_changed.connect(self._apply_theme)

    def _style(self):
        t = ui_tokens(self)
        return f"""
            QPushButton {{ padding: {dialog_scaled(4)}px {dialog_scaled(14)}px; color: {t.text_muted}; background: transparent;
                border: none; border-radius: {dialog_scaled(9)}px; font: {dialog_scaled(12)}px {FONT_FAMILY}; }}
            QPushButton:hover {{ color: {t.text}; background: {t.surface_subtle}; }}
            QPushButton:checked {{ color: {t.accent_text}; background: {t.accent_soft}; font-weight: 600; }}
        """

    def _apply_theme(self, _tokens=None):
        t = ui_tokens(self)
        set_own_style(self,
            f"background: {t.surface_subtle}; border: 1px solid {t.border}; "
            f"border-radius: {dialog_scaled(11)}px;"
        )
        for button in self._items.values():
            apply_style_sheet(button, self._style())

    def addItem(self, routeKey, text, onClick=None):
        self.insertItem(len(self._items), routeKey, text, onClick)

    def insertItem(self, index, routeKey, text, onClick=None):
        button = QPushButton(str(text), self)
        button.setCheckable(True)
        apply_style_sheet(button, self._style())
        # 选中项字重变粗；宽度按粗体量好，否则文字被裁
        button.setMinimumWidth(_bold_text_width(str(text)) + 2 * dialog_scaled(14) + 2)
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
