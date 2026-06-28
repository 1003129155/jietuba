"""Polished native input controls with stable application-facing names."""

from PySide6.QtWidgets import QComboBox, QDoubleSpinBox as _QDoubleSpinBox
from PySide6.QtWidgets import QLineEdit as _QLineEdit
from PySide6.QtWidgets import QSpinBox as _QSpinBox

from .theme import ACCENT, BORDER, BORDER_HOVER, FONT_FAMILY, SURFACE_STRONG, SURFACE_SUBTLE, TEXT


_INPUT_QSS = f"""
QComboBox, QLineEdit, QSpinBox, QDoubleSpinBox {{
    min-height: 26px; padding: 4px 11px; color: {TEXT};
    background: {SURFACE_STRONG}; border: 1px solid {BORDER}; border-radius: 10px;
    font: 13px {FONT_FAMILY}; selection-background-color: {ACCENT};
}}
QComboBox:hover, QLineEdit:hover, QSpinBox:hover, QDoubleSpinBox:hover {{
    border-color: {BORDER_HOVER}; background: rgba(255,255,255,.98);
}}
QComboBox:focus, QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus {{
    border: 1px solid {ACCENT}; background: rgba(255,255,255,.98);
}}
QComboBox:disabled, QLineEdit:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled {{
    color: #9AA39D; background: {SURFACE_SUBTLE}; border-color: #E6EBE7;
}}
QComboBox::drop-down {{ width: 24px; border: none; }}
QComboBox QAbstractItemView {{
    color: {TEXT}; background: #FFFFFF; border: 1px solid {BORDER};
    border-radius: 10px; padding: 5px; outline: none;
    selection-background-color: #DCEEFF; selection-color: {TEXT};
}}
QComboBox QAbstractItemView::item {{ min-height: 26px; padding: 3px 8px; }}
QSpinBox::up-button, QDoubleSpinBox::up-button,
QSpinBox::down-button, QDoubleSpinBox::down-button {{
    width: 18px; border: none; background: transparent;
}}
"""


class ComboBox(QComboBox):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setStyleSheet(_INPUT_QSS)
        self.setMinimumWidth(96)


class LineEdit(_QLineEdit):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setStyleSheet(_INPUT_QSS)


class SpinBox(_QSpinBox):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setStyleSheet(_INPUT_QSS)


class DoubleSpinBox(_QDoubleSpinBox):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setStyleSheet(_INPUT_QSS)
