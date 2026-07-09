"""Shared visual tokens for the lightweight UI component library."""

from __future__ import annotations

from PySide6.QtCore import QRectF, Qt
from PySide6.QtGui import QColor, QIcon, QLinearGradient, QPainter


FONT_FAMILY = '"Segoe UI Variable", "Microsoft YaHei UI", "Segoe UI", sans-serif'
# A restrained blue-grey accent.  Keeping this in one module prevents dialogs
# from quietly drifting back to unrelated Material/Windows blues.
ACCENT = "#6F8FAB"
ACCENT_HOVER = "#627F99"
ACCENT_PRESSED = "#526D85"
ACCENT_SOFT = "#DFE8EF"
ACCENT_SUBTLE = "rgba(111, 143, 171, 0.18)"
TEXT = "#17201B"
TEXT_MUTED = "#68736D"
BORDER = "rgba(112, 130, 119, 0.20)"
BORDER_HOVER = "rgba(76, 101, 86, 0.34)"
SURFACE = "rgba(255, 255, 255, 0.92)"
SURFACE_STRONG = "rgba(255, 255, 255, 0.98)"
SURFACE_SUBTLE = "rgba(248, 251, 253, 0.85)"
SURFACE_HOVER = "rgba(255, 255, 255, 1.0)"
FOCUS_RING = "rgba(111, 143, 171, 0.24)"

FROST_START = (226, 235, 242)
FROST_MIDDLE = (242, 245, 247)
FROST_END = (230, 237, 242)

# Opaque solid window background (no acrylic/blur).  Cheap to paint and stable
# regardless of the OS compositor or "transparency effects" setting.
# Kept clearly darker than the near-white card surfaces so cards stand out.
WINDOW_BG_TOP = (223, 231, 238)
WINDOW_BG_BOTTOM = (205, 216, 226)
WINDOW_BORDER = (180, 193, 205)


def to_qicon(icon) -> QIcon:
    """Convert FluentIcon/path/QIcon values to a QIcon."""
    if isinstance(icon, QIcon):
        return icon
    if icon is None:
        return QIcon()
    factory = getattr(icon, "icon", None)
    if callable(factory):
        return factory()
    return QIcon(str(icon))


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
    gradient.setColorAt(0.0, QColor(*WINDOW_BG_TOP))
    gradient.setColorAt(1.0, QColor(*WINDOW_BG_BOTTOM))

    painter.setCompositionMode(QPainter.CompositionMode.CompositionMode_SourceOver)
    painter.setBrush(gradient)
    painter.setPen(Qt.PenStyle.NoPen)
    painter.drawRoundedRect(rect, radius, radius)

    pen = painter.pen()
    pen.setColor(QColor(*WINDOW_BORDER))
    pen.setWidthF(1.0)
    painter.setPen(pen)
    painter.setBrush(Qt.BrushStyle.NoBrush)
    painter.drawRoundedRect(rect, radius, radius)
