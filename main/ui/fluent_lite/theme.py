"""Shared visual tokens for the lightweight UI component library."""

from __future__ import annotations

from PySide6.QtCore import QRectF, Qt
from functools import lru_cache

from PySide6.QtGui import QColor, QIcon, QLinearGradient, QPainter, QPixmap

from core.ui_theme import get_ui_theme


FONT_FAMILY = '"Segoe UI Variable", "Microsoft YaHei UI", "Segoe UI", sans-serif'
# A restrained blue-grey accent.  Keeping this in one module prevents dialogs
# from quietly drifting back to unrelated Material/Windows blues.
ACCENT = "#6F8FAB"
ACCENT_HOVER = "#627F99"
ACCENT_PRESSED = "#526D85"
ACCENT_SOFT = "#DFE8EF"
ACCENT_SUBTLE = "rgba(111, 143, 171, 0.18)"
FOCUS_RING = "rgba(111, 143, 171, 0.24)"

def ui_tokens(widget=None):
    """Return semantic colours, honoring an optional top-level theme scope."""
    current = widget
    while current is not None:
        override = getattr(current, "_ui_theme_tokens_override", None)
        if override is not None:
            return override
        parent_widget = getattr(current, "parentWidget", None)
        current = parent_widget() if callable(parent_widget) else None
    return get_ui_theme().tokens


def to_qicon(icon, widget=None) -> QIcon:
    """Convert an icon and tint SVG-backed UI icons for the current theme."""
    if isinstance(icon, QIcon):
        return icon
    if icon is None:
        return QIcon()
    icon_id = getattr(icon, "value", getattr(icon, "name", ""))
    preserve_color = str(icon_id).upper().endswith("WHITE")
    factory = getattr(icon, "icon", None)
    if callable(factory):
        source = factory()
        path = getattr(icon, "path", lambda: "")()
    else:
        path = str(icon)
        source = QIcon(path)
    if preserve_color or source.isNull():
        return source
    return _tinted_icon(path, ui_tokens(widget).text)


@lru_cache(maxsize=128)
def _tinted_icon(path: str, color: str) -> QIcon:
    source = QIcon(path)
    pixmap = source.pixmap(64, 64)
    if pixmap.isNull():
        return source
    tinted = QPixmap(pixmap.size())
    tinted.fill(Qt.GlobalColor.transparent)
    painter = QPainter(tinted)
    painter.drawPixmap(0, 0, pixmap)
    painter.setCompositionMode(
        QPainter.CompositionMode.CompositionMode_SourceIn
    )
    painter.fillRect(tinted.rect(), QColor(color))
    painter.end()
    return QIcon(tinted)


def paint_solid_background(widget, radius: int = 8) -> None:
    """Paint an opaque rounded solid background for frameless dialogs.

    Replaces the former acrylic/blur detection path.  A subtle vertical
    gradient plus a hairline border keeps it looking polished while staying
    cheap to repaint.
    """
    painter = QPainter(widget)
    painter.setRenderHint(QPainter.RenderHint.Antialiasing)
    painter.setCompositionMode(QPainter.CompositionMode.CompositionMode_Source)
    painter.fillRect(widget.rect(), Qt.GlobalColor.transparent)

    rect = QRectF(widget.rect()).adjusted(0.5, 0.5, -0.5, -0.5)

    gradient = QLinearGradient(0, 0, 0, widget.height())
    tokens = ui_tokens(widget)
    gradient.setColorAt(0.0, QColor(tokens.window_top))
    gradient.setColorAt(1.0, QColor(tokens.window_bottom))

    painter.setCompositionMode(QPainter.CompositionMode.CompositionMode_SourceOver)
    painter.setBrush(gradient)
    painter.setPen(Qt.PenStyle.NoPen)
    painter.drawRoundedRect(rect, radius, radius)

    pen = painter.pen()
    pen.setColor(QColor(tokens.window_border))
    pen.setWidthF(1.0)
    painter.setPen(pen)
    painter.setBrush(Qt.BrushStyle.NoBrush)
    painter.drawRoundedRect(rect, radius, radius)
