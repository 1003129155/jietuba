"""Polished native input controls with stable application-facing names."""

from PySide6.QtWidgets import QComboBox, QDoubleSpinBox as _QDoubleSpinBox
from PySide6.QtWidgets import QLineEdit as _QLineEdit
from PySide6.QtWidgets import QSpinBox as _QSpinBox
from PySide6.QtWidgets import QTextEdit as _QTextEdit

from core.ui_scale import widget_scaled as _px
from core.ui_theme import get_ui_theme

from .text_context_menu import TextContextMenuMixin, install_text_context_menu
from .theme import ACCENT, FONT_FAMILY, ui_tokens


def _input_qss(widget=None):
    t = ui_tokens(widget)
    s = lambda value: _px(widget, value)
    return f"""
    QComboBox, QLineEdit, QSpinBox, QDoubleSpinBox {{
        min-height: {s(26)}px; padding: {s(4)}px {s(11)}px; color: {t.text};
        background: {t.input_background}; border: 1px solid {t.border}; border-radius: {s(10)}px;
        font: {s(13)}px {FONT_FAMILY}; selection-background-color: {ACCENT};
    }}
    QComboBox:hover, QLineEdit:hover, QSpinBox:hover, QDoubleSpinBox:hover {{
        border-color: {t.border_hover}; background: {t.surface_strong};
    }}
    QComboBox:focus, QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus {{
        border: 1px solid {ACCENT}; background: {t.surface_strong};
    }}
    QComboBox:disabled, QLineEdit:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled {{
        color: {t.text_disabled}; background: {t.surface_subtle}; border-color: {t.border};
    }}
    QComboBox::drop-down {{ width: {s(24)}px; border: none; }}
    QComboBox QAbstractItemView {{
        color: {t.text}; background: {t.popup_background}; border: 1px solid {t.border};
        border-radius: {s(10)}px; padding: {s(5)}px; outline: none;
        selection-background-color: {t.accent_soft}; selection-color: {t.text};
    }}
    QComboBox QAbstractItemView::item {{ min-height: {s(26)}px; padding: {s(3)}px {s(8)}px; }}
    QSpinBox::up-button, QDoubleSpinBox::up-button,
    QSpinBox::down-button, QDoubleSpinBox::down-button {{
        width: {s(18)}px; border: none; background: transparent;
    }}
    """


class _ThemedInput:
    def _init_theme(self):
        self._apply_theme()
        get_ui_theme().theme_changed.connect(self._apply_theme)

    def _apply_theme(self, _tokens=None):
        self.setStyleSheet(_input_qss(self))


class ComboBox(_ThemedInput, QComboBox):
    # Drop-downs need room for their popup arrow plus a readable entry.
    BASE_MIN_WIDTH = 96

    def __init__(self, parent=None):
        super().__init__(parent)
        self._init_theme()

    def _apply_theme(self, _tokens=None):
        super()._apply_theme(_tokens)
        # The scaled floor must not push past a ceiling the caller pinned with
        # setFixedWidth: when min exceeds max Qt honours min, and the box grows
        # out of line with the rest of its column.
        self.setMinimumWidth(min(_px(self, self.BASE_MIN_WIDTH), self.maximumWidth()))


class LineEdit(TextContextMenuMixin, _ThemedInput, _QLineEdit):
    def __init__(self, parent=None, *, use_default_style: bool = True):
        super().__init__(parent)
        if use_default_style:
            self._init_theme()


class TextEdit(TextContextMenuMixin, _QTextEdit):
    """QTextEdit with the shared app context menu and caller-owned body style."""


class SpinBox(_ThemedInput, _QSpinBox):
    def __init__(self, parent=None):
        super().__init__(parent)
        self._init_theme()
        install_text_context_menu(self.lineEdit())


class DoubleSpinBox(_ThemedInput, _QDoubleSpinBox):
    def __init__(self, parent=None):
        super().__init__(parent)
        self._init_theme()
        install_text_context_menu(self.lineEdit())
