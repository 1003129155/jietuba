"""Project-owned, pure PySide6 replacement for the used Fluent widgets."""

from PySide6.QtCore import QRectF, Qt
from PySide6.QtGui import QColor, QPainter, QPalette
from PySide6.QtWidgets import QRadioButton as _QRadioButton, QStyle, QStyleOptionButton

from core.ui_theme import get_ui_theme

from .buttons import (
    HyperlinkButton, PrimaryPushButton, PushButton, TransparentPushButton,
    TransparentToolButton,
)
from .cards import SettingCard, SettingCardGroup, SimpleCardWidget, SwitchSettingCard
from .frameless import FramelessDialog, FrostedFramelessDialog
from .icons import FluentIcon
from .inputs import ComboBox, DoubleSpinBox, LineEdit, SpinBox, TextEdit
from .labels import BodyLabel, CaptionLabel
from .navigation import NavigationInterface, NavigationItemPosition
from .segmented import SegmentedWidget
from .switch import SwitchButton
from .theme import ACCENT, FONT_FAMILY, ui_tokens
from .titlebar import FluentTitleBar


class RadioButton(_QRadioButton):
    _LABEL_SPACING = 7

    def __init__(self, text="", parent=None):
        super().__init__(text, parent)
        self._apply_theme()
        get_ui_theme().theme_changed.connect(self._apply_theme)

    def _apply_theme(self, _tokens=None):
        self.setStyleSheet(
            f"QRadioButton {{ color: {ui_tokens(self).text}; "
            f"spacing: {self._LABEL_SPACING}px; font: 13px {FONT_FAMILY}; }}"
            "QRadioButton::indicator { width: 16px; height: 16px; }"
        )

    def _label_rect(self, indicator):
        """Return a text rect that can never overlap the custom indicator."""
        label_rect = self.rect()
        if self.layoutDirection() == Qt.LayoutDirection.RightToLeft:
            label_rect.setRight(indicator.left() - self._LABEL_SPACING - 1)
        else:
            label_rect.setLeft(indicator.right() + self._LABEL_SPACING + 1)
        return label_rect

    @staticmethod
    def _indicator_ellipse(indicator):
        """Keep the antialiased one-pixel outline inside the indicator box."""
        center = QRectF(indicator).center()
        return center, QRectF(center.x() - 7.0, center.y() - 7.0, 14.0, 14.0)

    def paintEvent(self, event):
        """Draw a crisp dot instead of Qt's thick, square-looking QSS ring."""
        option = QStyleOptionButton()
        self.initStyleOption(option)
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)

        indicator = self.style().subElementRect(
            QStyle.SubElement.SE_RadioButtonIndicator, option, self
        )
        # Some Windows/QSS combinations calculate CE_RadioButtonLabel from the
        # full widget rect and paint the text underneath the indicator.  Draw
        # the label in an explicitly separated rect so translations and DPI
        # scaling cannot make the two regions collide.
        label_rect = self._label_rect(indicator)
        text_flags = Qt.AlignmentFlag.AlignVCenter
        text_flags |= (
            Qt.AlignmentFlag.AlignRight
            if self.layoutDirection() == Qt.LayoutDirection.RightToLeft
            else Qt.AlignmentFlag.AlignLeft
        )
        text_flags |= Qt.TextFlag.TextShowMnemonic
        painter.setPen(option.palette.color(QPalette.ColorRole.WindowText))
        painter.drawText(label_rect, text_flags, option.text)

        center, outer = self._indicator_ellipse(indicator)
        tokens = ui_tokens(self)
        painter.setPen(QColor(ACCENT if self.isChecked() or self.underMouse() else tokens.border_hover))
        painter.setBrush(QColor(ACCENT if self.isChecked() else tokens.input_background))
        painter.drawEllipse(outer)
        if self.isChecked():
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(QColor("#FFFFFF"))
            painter.drawEllipse(QRectF(center.x() - 3.0, center.y() - 3.0, 6.0, 6.0))


__all__ = [
    "PushButton", "PrimaryPushButton", "TransparentPushButton", "TransparentToolButton",
    "HyperlinkButton", "BodyLabel", "CaptionLabel", "ComboBox", "LineEdit", "TextEdit", "SpinBox",
    "DoubleSpinBox", "RadioButton", "SwitchButton", "SegmentedWidget", "FluentIcon",
    "SettingCard", "SwitchSettingCard", "SettingCardGroup", "SimpleCardWidget",
    "NavigationInterface", "NavigationItemPosition", "FluentTitleBar", "FramelessDialog",
    "FrostedFramelessDialog",
]
