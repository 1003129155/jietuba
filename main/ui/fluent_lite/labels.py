"""Typography-compatible label classes."""

from PySide6.QtWidgets import QLabel, QSizePolicy, QWidget

from core.ui_theme import get_ui_theme

from .theme import FONT_FAMILY, ui_tokens


def _parse_args(args, kwargs):
    text, parent = "", None
    if len(args) == 1:
        if isinstance(args[0], QWidget):
            parent = args[0]
        else:
            text = "" if args[0] is None else str(args[0])
    elif len(args) >= 2:
        text = "" if args[0] is None else str(args[0])
        parent = args[1]
    return str(kwargs.get("text", text)), kwargs.get("parent", parent)


class BodyLabel(QLabel):
    def __init__(self, *args, **kwargs):
        text, parent = _parse_args(args, kwargs)
        super().__init__(text, parent)
        self._apply_theme()
        get_ui_theme().theme_changed.connect(self._apply_theme)

    def _apply_theme(self, _tokens=None):
        self.setStyleSheet(
            f"background: transparent; color: {ui_tokens(self).text}; "
            f"font: 13px {FONT_FAMILY};"
        )


class CaptionLabel(QLabel):
    def __init__(self, *args, **kwargs):
        text, parent = _parse_args(args, kwargs)
        super().__init__(text, parent)
        self._apply_theme()
        get_ui_theme().theme_changed.connect(self._apply_theme)
        self.setWordWrap(True)
        self.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Preferred)

    def _apply_theme(self, _tokens=None):
        self.setStyleSheet(
            f"background: transparent; color: {ui_tokens(self).text_muted}; "
            f"font: 12px {FONT_FAMILY};"
        )
