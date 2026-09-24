# -*- coding: utf-8 -*-

"""管理窗口右栏表单共用的小控件。"""

from PySide6.QtCore import QRectF, QSize, Qt
from PySide6.QtGui import QFont, QFontMetrics, QPainter, QPen
from PySide6.QtWidgets import QAbstractButton, QLabel, QSizePolicy

from core import safe_event
from core.ui_scale import dialog_scaled, dialog_scaled_f
from core.ui_theme import get_ui_theme
from ui.fluent_lite.theme import css_color
from ui.settings_ui.components import apply_theme_text_style


def field_label(text: str, *, top_gap: bool = True) -> QLabel:
    """输入框上方的字段名。"""
    label = QLabel(text)
    # 间距用 contentsMargins 给：样式表里写 padding 会让 QLabel 的文字多缩进几像素，和上下不对齐。
    label.setContentsMargins(0, dialog_scaled(8) if top_gap else 0, 0, 0)
    apply_theme_text_style(label, 12, extra="font-weight: 600;")
    return label


def field_hint(text: str) -> QLabel:
    label = QLabel(text)
    label.setWordWrap(True)
    apply_theme_text_style(label, 11, caption=True)
    return label


class OptionCard(QAbstractButton):
    """带标题和一行说明的可选卡片，放进 QButtonGroup 里当单选用。"""

    def __init__(self, title: str, description: str, parent=None):
        super().__init__(parent)
        self._title = title
        self._description = description
        self.setText(title)
        self.setCheckable(True)
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        self.setMouseTracking(True)
        get_ui_theme().theme_changed.connect(self.update)

    def sizeHint(self) -> QSize:
        return QSize(dialog_scaled(150), dialog_scaled(58))

    def minimumSizeHint(self) -> QSize:
        return QSize(dialog_scaled(96), dialog_scaled(58))

    def _fonts(self):
        title = QFont(self.font())
        title.setPixelSize(dialog_scaled(13))
        title.setWeight(QFont.Weight.DemiBold)
        description = QFont(self.font())
        description.setPixelSize(dialog_scaled(11))
        return title, description

    @safe_event
    def paintEvent(self, _event):
        tokens = get_ui_theme().tokens
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        rect = QRectF(self.rect()).adjusted(0.5, 0.5, -0.5, -0.5)
        radius = dialog_scaled_f(10)

        checked = self.isChecked()
        if checked:
            painter.setBrush(css_color(tokens.accent_soft))
            painter.setPen(QPen(css_color(tokens.accent), dialog_scaled_f(1.5)))
        else:
            painter.setBrush(css_color(tokens.surface_hover if self.underMouse() else tokens.input_background))
            painter.setPen(QPen(css_color(tokens.border_hover if self.underMouse() else tokens.border), 1))
        painter.drawRoundedRect(rect, radius, radius)

        title_font, description_font = self._fonts()
        pad_x = dialog_scaled(12)
        inner = rect.adjusted(pad_x, dialog_scaled(10), -pad_x, -dialog_scaled(10))
        text_w = int(inner.width())
        half = inner.height() / 2
        painter.setFont(title_font)
        painter.setPen(css_color(tokens.accent_text if checked else tokens.text))
        painter.drawText(
            QRectF(inner.left(), inner.top(), text_w, half),
            Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter,
            QFontMetrics(title_font).elidedText(self._title, Qt.TextElideMode.ElideRight, text_w),
        )
        painter.setFont(description_font)
        painter.setPen(css_color(tokens.text_muted))
        painter.drawText(
            QRectF(inner.left(), inner.top() + half, inner.width(), half),
            Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter,
            QFontMetrics(description_font).elidedText(
                self._description, Qt.TextElideMode.ElideRight, int(inner.width())
            ),
        )
        painter.end()

    def enterEvent(self, event):
        self.update()
        super().enterEvent(event)

    def leaveEvent(self, event):
        self.update()
        super().leaveEvent(event)
