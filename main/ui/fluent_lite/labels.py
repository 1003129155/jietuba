"""Typography-compatible label classes."""

from PySide6.QtWidgets import QLabel, QSizePolicy, QWidget

from .theme import FONT_FAMILY, TEXT, TEXT_MUTED


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
        self.setStyleSheet(
            f"background: transparent; color: {TEXT}; font: 13px {FONT_FAMILY};"
        )


class CaptionLabel(QLabel):
    def __init__(self, *args, **kwargs):
        text, parent = _parse_args(args, kwargs)
        super().__init__(text, parent)
        self.setStyleSheet(
            f"background: transparent; color: {TEXT_MUTED}; font: 12px {FONT_FAMILY};"
        )
        self.setWordWrap(True)
        self.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Preferred)
