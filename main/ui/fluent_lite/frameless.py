"""Shared frameless/frosted window foundation for application dialogs."""

from PySide6.QtCore import Qt
from qframelesswindow import FramelessDialog

from .theme import apply_frosted_backdrop, paint_frosted_background


class FrostedFramelessDialog(FramelessDialog):
    """Frameless dialog with one consistent native/fallback glass backdrop."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, True)
        self._native_backdrop_enabled = False
        self._frosted_corner_radius = 18
        self._last_backdrop_status = None

    def paintEvent(self, event):
        paint_frosted_background(
            self, self._native_backdrop_enabled, self._frosted_corner_radius
        )

    def showEvent(self, event):
        super().showEvent(event)
        self._native_backdrop_enabled = apply_frosted_backdrop(self)
        status = "enabled" if self._native_backdrop_enabled else "fallback"
        if status != self._last_backdrop_status:
            try:
                from core.logger import log_debug
                log_debug(
                    f"{type(self).__name__}: native acrylic={status}",
                    "GlassBackdrop",
                )
            except Exception:
                pass
            self._last_backdrop_status = status
        self.update()


__all__ = ["FramelessDialog", "FrostedFramelessDialog"]
