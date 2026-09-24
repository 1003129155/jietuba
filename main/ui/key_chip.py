# -*- coding: utf-8 -*-
"""快捷键按键块与状态图标。

全局热键录入框和应用内快捷键录入框共用这里的外观：配置值保持原样
（"ctrl+1"、"mouseforward"），只在绘制时换成 "Ctrl + 1"、"鼠标前进键"。
"""
from PySide6.QtCore import QCoreApplication, QPointF, QRectF, QSize, Qt
from PySide6.QtGui import QFont, QFontMetrics, QPainter, QPainterPath, QPen
from PySide6.QtWidgets import QLineEdit, QWidget

from core import safe_event
from core.i18n import make_tr
from core.shortcut_manager import (
    MOUSE_BUTTON_BACK, MOUSE_BUTTON_FORWARD, MOUSE_BUTTON_MIDDLE,
    get_key_display_map, get_key_parse_map,
)
from core.ui_scale import dialog_scaled
from core.ui_theme import get_ui_theme
from ui.fluent_lite.theme import css_color

_tr = make_tr("KeyChip")

CHIP_WIDTH = 196
CHIP_HEIGHT = 28
STATUS_SIZE = 18
STATUS_GAP = 12

_MODIFIER_NAMES = {
    "ctrl": "Ctrl", "shift": "Shift", "alt": "Alt", "win": "Win", "meta": "Win",
}


def _part_display(part: str) -> str:
    if part in _MODIFIER_NAMES:
        return _MODIFIER_NAMES[part]
    if part == MOUSE_BUTTON_BACK:
        return _tr("Mouse Back")
    if part == MOUSE_BUTTON_FORWARD:
        return _tr("Mouse Forward")
    if part == MOUSE_BUTTON_MIDDLE:
        return QCoreApplication.translate("InAppShortcut", "Middle")
    qt_key = get_key_parse_map().get(part)
    if qt_key is not None and qt_key in get_key_display_map():
        return get_key_display_map()[qt_key]
    # 字母、数字、F 键全大写；其余少见的键名只首字母大写。
    return part.upper() if len(part) <= 3 else part.capitalize()


def shortcut_display_parts(value: str) -> tuple[list[str], bool]:
    """配置值 → (各段显示名, 是否是 "ctrl+" 这种还没按完的前缀)。"""
    text = (value or "").strip().lower()
    if not text:
        return [], False
    pending = text.endswith("+") and not text.endswith("++")
    parts = [part for part in text.split("+") if part]
    if text.endswith("++"):
        parts.append("+")
    return [_part_display(part) for part in parts], pending


def format_shortcut_text(value: str) -> str:
    parts, pending = shortcut_display_parts(value)
    return " + ".join(parts) + (" +" if pending else "")


class KeyChipLineEdit(QLineEdit):
    """把快捷键画成按键块的录入框。

    仍然继承 QLineEdit：ShortcutManager 靠 isinstance(QLineEdit) 判断焦点在
    文字输入上，录入期间才不会被截图、钉图的快捷键抢走按键。
    """

    def __init__(self, parent=None):
        super().__init__(parent)
        self._error = False
        self._hover_clear = False
        self.setMouseTracking(True)
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setFixedHeight(dialog_scaled(CHIP_HEIGHT))
        get_ui_theme().theme_changed.connect(self.update)

    def sizeHint(self):
        return QSize(dialog_scaled(CHIP_WIDTH), dialog_scaled(CHIP_HEIGHT))

    def minimumSizeHint(self):
        return QSize(dialog_scaled(96), dialog_scaled(CHIP_HEIGHT))

    def setErrorState(self, error: bool):
        if self._error != bool(error):
            self._error = bool(error)
            self.update()

    def errorState(self) -> bool:
        return self._error

    def _clear_rect(self) -> QRectF:
        side = self.height()
        return QRectF(self.width() - side, 0, side, side)

    @safe_event
    def mouseMoveEvent(self, event):
        hover = bool(self.text()) and self._clear_rect().contains(event.position())
        if hover != self._hover_clear:
            self._hover_clear = hover
            self.update()
        super().mouseMoveEvent(event)

    @safe_event
    def leaveEvent(self, event):
        self._hover_clear = False
        self.update()
        super().leaveEvent(event)

    @safe_event
    def enterEvent(self, event):
        self.update()
        super().enterEvent(event)

    @safe_event
    def mousePressEvent(self, event):
        if (
            event.button() == Qt.MouseButton.LeftButton
            and self.text()
            and self._clear_rect().contains(event.position())
        ):
            event.accept()
            self.setText("")
            return
        super().mousePressEvent(event)

    @safe_event
    def paintEvent(self, event):
        t = get_ui_theme().tokens
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        font = QFont(self.font())
        font.setPixelSize(dialog_scaled(13))
        painter.setFont(font)
        metrics = QFontMetrics(font)

        radius = dialog_scaled(8)
        rect = QRectF(self.rect()).adjusted(0.5, 0.5, -0.5, -0.5)
        pad = dialog_scaled(12)
        focused = self.hasFocus()
        parts, pending = shortcut_display_parts(self.text())

        if not parts and not pending and not focused:
            border = css_color(t.text_muted)
            border.setAlphaF(0.55)
            pen = QPen(border, 1.0, Qt.PenStyle.DashLine)
            painter.setPen(pen)
            painter.setBrush(Qt.BrushStyle.NoBrush)
            painter.drawRoundedRect(rect, radius, radius)
            self._paint_plus(painter, QPointF(pad + dialog_scaled(5), rect.center().y()), css_color(t.text_muted))
            painter.setPen(css_color(t.text_muted))
            text_rect = rect.adjusted(pad + dialog_scaled(16), 0, -pad, 0)
            painter.drawText(
                text_rect,
                Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignLeft,
                metrics.elidedText(_tr("Add shortcut"), Qt.TextElideMode.ElideRight, int(text_rect.width())),
            )
            return

        if self._error:
            fill, border, width = css_color(t.danger_soft), css_color(t.danger), 1.0
        elif focused:
            fill, border, width = css_color(t.surface_hover), css_color(t.accent), 1.5
        else:
            # 深色主题下 input_background 和卡片底色相同，按键块会只剩一圈边框。
            fill = css_color(t.surface_hover)
            border = css_color(t.accent if self.underMouse() else t.border_hover)
            width = 1.0
        inset = width / 2
        painter.setPen(QPen(border, width))
        painter.setBrush(fill)
        painter.drawRoundedRect(QRectF(self.rect()).adjusted(inset, inset, -inset, -inset), radius, radius)

        clear = self._clear_rect()
        text_right = (clear.left() if self.text() else rect.right() - pad)
        available = int(text_right - pad)
        text_color, sep_color = css_color(t.text), css_color(t.text_muted)
        baseline = rect.center().y() + (metrics.ascent() - metrics.descent()) / 2

        if not parts:
            painter.setPen(sep_color)
            painter.drawText(
                QPointF(pad, baseline),
                metrics.elidedText(_tr("Press keys…"), Qt.TextElideMode.ElideRight, available),
            )
        else:
            sep = " + "
            full = sep.join(parts) + (" +" if pending else "")
            if metrics.horizontalAdvance(full) > available:
                painter.setPen(text_color)
                painter.drawText(
                    QPointF(pad, baseline),
                    metrics.elidedText(full, Qt.TextElideMode.ElideRight, available),
                )
            else:
                x = float(pad)
                for index, part in enumerate(parts):
                    if index:
                        painter.setPen(sep_color)
                        painter.drawText(QPointF(x, baseline), sep)
                        x += metrics.horizontalAdvance(sep)
                    painter.setPen(text_color)
                    painter.drawText(QPointF(x, baseline), part)
                    x += metrics.horizontalAdvance(part)
                if pending:
                    painter.setPen(sep_color)
                    painter.drawText(QPointF(x, baseline), " +")

        if self.text():
            color = css_color(t.text if self._hover_clear else t.text_muted)
            half = dialog_scaled(4)
            center = clear.center()
            painter.setPen(QPen(color, 1.4, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))
            painter.drawLine(QPointF(center.x() - half, center.y() - half), QPointF(center.x() + half, center.y() + half))
            painter.drawLine(QPointF(center.x() + half, center.y() - half), QPointF(center.x() - half, center.y() + half))

    @staticmethod
    def _paint_plus(painter, center: QPointF, color):
        half = dialog_scaled(5)
        painter.setPen(QPen(color, 1.5, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))
        painter.drawLine(QPointF(center.x() - half, center.y()), QPointF(center.x() + half, center.y()))
        painter.drawLine(QPointF(center.x(), center.y() - half), QPointF(center.x(), center.y() + half))


class StatusIcon(QWidget):
    """按键块右侧的状态圆点：可用 / 未设置 / 冲突，空串表示不显示。"""

    STATES = ("", "ok", "idle", "error")

    def __init__(self, parent=None):
        super().__init__(parent)
        self._state = ""
        side = dialog_scaled(STATUS_SIZE)
        self.setFixedSize(side, side)
        get_ui_theme().theme_changed.connect(self.update)

    @property
    def state(self) -> str:
        return self._state

    def setState(self, state: str, tooltip: str = ""):
        if state not in self.STATES:
            raise ValueError(state)
        self._state = state
        self.setToolTip(tooltip)
        self.update()

    @safe_event
    def paintEvent(self, event):
        if not self._state:
            return
        t = get_ui_theme().tokens
        color = {"ok": t.success, "idle": t.status_idle, "error": t.danger}[self._state]
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(css_color(color))
        side = float(min(self.width(), self.height()))
        painter.drawEllipse(QRectF(0, 0, side, side))

        painter.setPen(QPen(Qt.GlobalColor.white, max(1.6, side * 0.12), Qt.PenStyle.SolidLine,
                            Qt.PenCapStyle.RoundCap, Qt.PenJoinStyle.RoundJoin))
        painter.setBrush(Qt.BrushStyle.NoBrush)
        if self._state == "ok":
            path = QPainterPath(QPointF(side * 0.29, side * 0.52))
            path.lineTo(side * 0.44, side * 0.66)
            path.lineTo(side * 0.71, side * 0.37)
            painter.drawPath(path)
        elif self._state == "idle":
            painter.drawLine(QPointF(side * 0.31, side * 0.5), QPointF(side * 0.69, side * 0.5))
        else:
            a, b = side * 0.35, side * 0.65
            painter.drawLine(QPointF(a, a), QPointF(b, b))
            painter.drawLine(QPointF(b, a), QPointF(a, b))
