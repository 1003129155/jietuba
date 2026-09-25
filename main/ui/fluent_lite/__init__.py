"""Project-owned, pure PySide6 replacement for the used Fluent widgets."""

from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import QColor, QPainter, QPainterPath, QPalette, QPen
from PySide6.QtWidgets import (
    QCheckBox as _QCheckBox, QRadioButton as _QRadioButton, QStyle, QStyleOptionButton,
)

from core.ui_scale import widget_scaled as _px
from core.ui_theme import get_ui_theme

from .buttons import (
    ColorSwatchButton, HyperlinkButton, PrimaryPushButton, PushButton,
    TransparentPushButton,
    TransparentToolButton,
)
from .cards import (
    SettingCard, SettingCardGroup, SimpleCardWidget, SwitchSettingCard,
    card_right_margin,
)
from .frameless import FramelessDialog, FrostedFramelessDialog
from .icons import FluentIcon
from .inputs import ComboBox, DoubleSpinBox, LineEdit, SpinBox, TextEdit
from .labels import BodyLabel, CaptionLabel
from .navigation import NavigationInterface, NavigationItemPosition
from .segmented import SegmentedWidget
from .switch import SwitchButton
from .theme import (
    ACCENT, FONT_FAMILY, INPUT_HEIGHT, INPUT_RADIUS, scrollbar_qss, ui_tokens,
)
from .titlebar import FluentTitleBar


class RadioButton(_QRadioButton):
    _LABEL_SPACING = 7

    def __init__(self, text="", parent=None):
        super().__init__(text, parent)
        self._apply_theme()
        get_ui_theme().theme_changed.connect(self._apply_theme)

    def _apply_theme(self, _tokens=None):
        spacing = _px(self, self._LABEL_SPACING)
        self.setStyleSheet(
            f"QRadioButton {{ color: {ui_tokens(self).text}; "
            f"spacing: {spacing}px; font: {_px(self, 13)}px {FONT_FAMILY}; }}"
            f"QRadioButton::indicator {{ width: {_px(self, 16)}px; height: {_px(self, 16)}px; }}"
        )

    def _label_rect(self, indicator):
        """Return a text rect that can never overlap the custom indicator."""
        label_rect = self.rect()
        spacing = _px(self, self._LABEL_SPACING)
        if self.layoutDirection() == Qt.LayoutDirection.RightToLeft:
            label_rect.setRight(indicator.left() - spacing - 1)
        else:
            label_rect.setLeft(indicator.right() + spacing + 1)
        return label_rect

    def _indicator_ellipse(self, indicator):
        """Keep the antialiased one-pixel outline inside the indicator box."""
        center = QRectF(indicator).center()
        diameter = float(_px(self, 14))
        radius = diameter / 2.0
        return center, QRectF(center.x() - radius, center.y() - radius, diameter, diameter)

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
            dot_diameter = float(_px(self, 6))
            dot_radius = dot_diameter / 2.0
            painter.drawEllipse(
                QRectF(
                    center.x() - dot_radius,
                    center.y() - dot_radius,
                    dot_diameter,
                    dot_diameter,
                )
            )


class CheckBox(_QCheckBox):
    """勾选框。和 RadioButton 一样自己画指示器和文字，避开 QSS 方框发虚、文字压住方框的问题"""

    _LABEL_SPACING = 7

    def __init__(self, text="", parent=None):
        super().__init__(text, parent)
        self._apply_theme()
        get_ui_theme().theme_changed.connect(self._apply_theme)

    def _apply_theme(self, _tokens=None):
        spacing = _px(self, self._LABEL_SPACING)
        self.setStyleSheet(
            f"QCheckBox {{ color: {ui_tokens(self).text}; background: transparent; "
            f"spacing: {spacing}px; font: {_px(self, 13)}px {FONT_FAMILY}; }}"
            f"QCheckBox::indicator {{ width: {_px(self, 16)}px; height: {_px(self, 16)}px; }}"
        )

    def paintEvent(self, event):
        option = QStyleOptionButton()
        self.initStyleOption(option)
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)

        indicator = self.style().subElementRect(
            QStyle.SubElement.SE_CheckBoxIndicator, option, self
        )
        label_rect = self.rect()
        spacing = _px(self, self._LABEL_SPACING)
        text_flags = Qt.AlignmentFlag.AlignVCenter | Qt.TextFlag.TextShowMnemonic
        if self.layoutDirection() == Qt.LayoutDirection.RightToLeft:
            label_rect.setRight(indicator.left() - spacing - 1)
            text_flags |= Qt.AlignmentFlag.AlignRight
        else:
            label_rect.setLeft(indicator.right() + spacing + 1)
            text_flags |= Qt.AlignmentFlag.AlignLeft
        painter.setPen(option.palette.color(QPalette.ColorRole.WindowText))
        painter.drawText(label_rect, text_flags, option.text)

        tokens = ui_tokens(self)
        checked = self.isChecked()
        box = QRectF(indicator).adjusted(0.5, 0.5, -0.5, -0.5)
        radius = float(_px(self, 4))
        painter.setPen(QColor(ACCENT if checked or self.underMouse() else tokens.border_hover))
        painter.setBrush(QColor(ACCENT if checked else tokens.input_background))
        painter.drawRoundedRect(box, radius, radius)
        if checked:
            pen = QPen(QColor("#FFFFFF"), max(1.5, box.width() / 9))
            pen.setCapStyle(Qt.PenCapStyle.RoundCap)
            pen.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
            painter.setPen(pen)
            painter.setBrush(Qt.BrushStyle.NoBrush)
            x, y, w, h = box.x(), box.y(), box.width(), box.height()
            tick = QPainterPath(QPointF(x + w * 0.26, y + h * 0.52))
            tick.lineTo(x + w * 0.43, y + h * 0.69)
            tick.lineTo(x + w * 0.75, y + h * 0.34)
            painter.drawPath(tick)


__all__ = [
    "PushButton", "PrimaryPushButton", "TransparentPushButton", "TransparentToolButton",
    "ColorSwatchButton",
    "HyperlinkButton", "BodyLabel", "CaptionLabel", "ComboBox", "LineEdit", "TextEdit", "SpinBox",
    "DoubleSpinBox", "RadioButton", "CheckBox", "SwitchButton", "SegmentedWidget", "FluentIcon",
    "SettingCard", "SwitchSettingCard", "SettingCardGroup", "SimpleCardWidget",
    "card_right_margin",
    "NavigationInterface", "NavigationItemPosition", "FluentTitleBar", "FramelessDialog",
    "FrostedFramelessDialog", "scrollbar_qss",
    "INPUT_HEIGHT", "INPUT_RADIUS",
]
