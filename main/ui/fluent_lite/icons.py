"""Local SVG-backed icon namespace used by the application UI."""

from __future__ import annotations

from enum import Enum
from pathlib import Path
import sys

from PySide6.QtGui import QIcon


def _icon_dir() -> Path:
    # PyInstaller exposes bundled data below _MEIPASS.  In a source checkout
    # this module lives at <repo>/main/ui/fluent_lite.
    frozen_root = getattr(sys, "_MEIPASS", None)
    if frozen_root:
        return Path(frozen_root) / "svg" / "fluent_icons"
    return Path(__file__).resolve().parents[3] / "svg" / "fluent_icons"


_ICON_DIR = _icon_dir()


class FluentIcon(Enum):
    COMMAND_PROMPT = "CommandPrompt"
    CAMERA = "Camera"
    PASTE = "Paste"
    BRUSH = "Brush"
    LANGUAGE = "Language"
    HISTORY = "History"
    APPLICATION = "Application"
    INFO = "Info"
    DELETE = "Delete"
    CLOSE = "Close"
    CANCEL = "CancelClose"
    CHECK = "CheckWhite"
    SPARKLE = "SparkleWhite"
    SAVE = "Save"
    FILTER = "Filter"
    DATE_TIME = "DateTime"
    SEND = "Send"
    FOLDER = "Folder"
    EDIT = "Edit"
    DOWNLOAD = "Download"
    PALETTE = "Palette"
    FONT_SIZE = "FontSize"
    TRANSPARENT = "Transparent"
    CERTIFICATE = "Certificate"
    ALIGNMENT = "Alignment"
    DOCUMENT = "Document"
    POWER_BUTTON = "PowerButton"
    PIN = "Pin"
    SETTING = "Setting"
    STOP_WATCH = "StopWatch"
    FONT = "Font"
    LAYOUT = "Layout"
    SEARCH = "Search"
    SYNC = "Sync"
    HIDE = "Hide"
    PEOPLE = "People"
    GITHUB = "GitHub"

    def path(self) -> str:
        return str(_ICON_DIR / f"{self.value}.svg")

    def icon(self) -> QIcon:
        return QIcon(self.path())
