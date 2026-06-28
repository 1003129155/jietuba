"""Shared visual tokens for the lightweight UI component library."""

from __future__ import annotations

import sys

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
SURFACE = "rgba(255, 255, 255, 0.74)"
SURFACE_STRONG = "rgba(255, 255, 255, 0.90)"
SURFACE_SUBTLE = "rgba(244, 248, 245, 0.62)"
SURFACE_HOVER = "rgba(255, 255, 255, 0.92)"
FOCUS_RING = "rgba(111, 143, 171, 0.24)"

FROST_START = (226, 235, 242)
FROST_MIDDLE = (242, 245, 247)
FROST_END = (230, 237, 242)


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


def apply_frosted_backdrop(widget) -> bool:
    """Enable the native Windows acrylic backdrop when available.

    The UI remains fully usable on older Windows versions and other platforms;
    its translucent QSS surfaces provide the visual fallback.
    """
    if sys.platform != "win32":
        return False

    # Reuse qframelesswindow's version-aware implementation.  It configures
    # blur-behind, Acrylic tint, animation and shadow as one coherent setup.
    effect = getattr(widget, "windowEffect", None)
    if effect is not None:
        try:
            effect.enableBlurBehindWindow(widget.winId())
            effect.setAcrylicEffect(
                widget.winId(), gradientColor="E9EFF366", enableShadow=True
            )
            return True
        except Exception:
            pass

    try:
        import ctypes

        hwnd = int(widget.winId())
        value = ctypes.c_int(3)  # DWMSBT_TRANSIENTWINDOW (Acrylic)
        result = ctypes.windll.dwmapi.DwmSetWindowAttribute(
            hwnd, 38, ctypes.byref(value), ctypes.sizeof(value)
        )
        corner = ctypes.c_int(2)  # DWMWCP_ROUND
        ctypes.windll.dwmapi.DwmSetWindowAttribute(
            hwnd, 33, ctypes.byref(corner), ctypes.sizeof(corner)
        )
        if result == 0:
            return True
    except Exception:
        pass

    # Windows 10 fallback using SetWindowCompositionAttribute.
    try:
        import ctypes

        class AccentPolicy(ctypes.Structure):
            _fields_ = [
                ("state", ctypes.c_int),
                ("flags", ctypes.c_int),
                ("gradient_color", ctypes.c_uint),
                ("animation_id", ctypes.c_int),
            ]

        class CompositionData(ctypes.Structure):
            _fields_ = [
                ("attribute", ctypes.c_int),
                ("data", ctypes.c_void_p),
                ("size", ctypes.c_size_t),
            ]

        policy = AccentPolicy(4, 2, 0xCCF3EFE9, 0)
        data = CompositionData(19, ctypes.addressof(policy), ctypes.sizeof(policy))
        return bool(ctypes.windll.user32.SetWindowCompositionAttribute(int(widget.winId()), ctypes.byref(data)))
    except Exception:
        return False


def paint_frosted_background(widget, native_enabled: bool = False, radius: int = 18) -> None:
    """Paint the shared translucent fallback used by all large dialogs."""
    painter = QPainter(widget)
    painter.setRenderHint(QPainter.RenderHint.Antialiasing)
    painter.setCompositionMode(QPainter.CompositionMode.CompositionMode_Source)
    painter.fillRect(widget.rect(), Qt.GlobalColor.transparent)

    edge_alpha = 148 if native_enabled else 238
    middle_alpha = 132 if native_enabled else 232
    gradient = QLinearGradient(0, 0, widget.width(), widget.height())
    gradient.setColorAt(0.0, QColor(*FROST_START, edge_alpha))
    gradient.setColorAt(0.48, QColor(*FROST_MIDDLE, middle_alpha))
    gradient.setColorAt(1.0, QColor(*FROST_END, edge_alpha))
    painter.setBrush(gradient)
    painter.setPen(Qt.PenStyle.NoPen)

    if native_enabled:
        # DWM already clips the window and supplies its shadow.  Drawing a
        # second Qt rounded rect here produces a visibly offset inner corner.
        painter.drawRect(widget.rect())
        return

    # Non-native fallback owns the sole window outline.  Keep its antialiased
    # edge inside the device without another border stroke.
    rect = QRectF(widget.rect()).adjusted(0.5, 0.5, -0.5, -0.5)
    painter.drawRoundedRect(rect, radius, radius)
