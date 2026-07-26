# -*- coding: utf-8 -*-
"""Application-wide light/dark appearance management.

This module intentionally does not manage screenshot accent and mask colours;
those remain in :mod:`core.theme`.  UIThemeManager owns only semantic colours
used by application windows and native Qt widgets.
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Optional

from PySide6.QtCore import QObject, Qt, Signal
from PySide6.QtGui import QColor, QGuiApplication, QPalette
from PySide6.QtWidgets import QApplication


class UIThemeMode(str, Enum):
    SYSTEM = "system"
    LIGHT = "light"
    DARK = "dark"

    @classmethod
    def coerce(cls, value) -> "UIThemeMode":
        if isinstance(value, cls):
            return value
        try:
            return cls(str(value).lower())
        except ValueError:
            return cls.SYSTEM


@dataclass(frozen=True)
class UIThemeTokens:
    mode: UIThemeMode
    accent: str
    accent_hover: str
    accent_pressed: str
    accent_soft: str
    window: str
    window_top: str
    window_bottom: str
    surface: str
    surface_strong: str
    surface_subtle: str
    surface_hover: str
    text: str
    text_muted: str
    text_disabled: str
    border: str
    border_hover: str
    window_border: str
    input_background: str
    popup_background: str
    popup_hover: str
    separator: str
    switch_off: str
    selected_text: str = "#FFFFFF"

    @property
    def is_dark(self) -> bool:
        return self.mode is UIThemeMode.DARK


_COMMON = {
    "accent": "#6F8FAB",
    "accent_hover": "#627F99",
    "accent_pressed": "#526D85",
}

LIGHT_TOKENS = UIThemeTokens(
    mode=UIThemeMode.LIGHT,
    **_COMMON,
    accent_soft="#DFE8EF",
    window="#D9E3EC",
    window_top="#DFE7EE",
    window_bottom="#CDD8E2",
    surface="rgba(255, 255, 255, 0.92)",
    surface_strong="rgba(255, 255, 255, 0.98)",
    surface_subtle="rgba(248, 251, 253, 0.85)",
    surface_hover="#FFFFFF",
    text="#17201B",
    text_muted="#68736D",
    text_disabled="#9AA39D",
    border="rgba(112, 130, 119, 0.20)",
    border_hover="rgba(76, 101, 86, 0.34)",
    window_border="#B4C1CD",
    input_background="#FFFFFF",
    popup_background="#FFFFFF",
    popup_hover="#EAF2FA",
    separator="rgba(98, 116, 105, 0.13)",
    switch_off="#C4CCC6",
)

DARK_TOKENS = UIThemeTokens(
    mode=UIThemeMode.DARK,
    **_COMMON,
    accent_soft="#31404C",
    window="#1B1E22",
    window_top="#24282D",
    window_bottom="#191C20",
    surface="rgba(43, 47, 52, 0.96)",
    surface_strong="rgba(49, 54, 60, 0.99)",
    surface_subtle="rgba(36, 40, 45, 0.92)",
    surface_hover="#373C43",
    text="#F2F4F6",
    text_muted="#AEB5BD",
    text_disabled="#737B84",
    border="rgba(255, 255, 255, 0.12)",
    border_hover="rgba(255, 255, 255, 0.28)",
    window_border="#3B424A",
    input_background="#2B2F34",
    popup_background="#292D32",
    popup_hover="#363B42",
    separator="rgba(255, 255, 255, 0.10)",
    switch_off="#626A73",
)


class UIThemeManager(QObject):
    """Resolve, persist and apply the effective application appearance."""

    theme_changed = Signal(object)
    mode_changed = Signal(str)

    def __init__(self, parent=None):
        super().__init__(parent)
        self._config = None
        self._application: Optional[QApplication] = None
        self._mode = UIThemeMode.SYSTEM
        self._effective_mode = UIThemeMode.LIGHT
        self._system_signal_connected = False

    def init(self, config_manager=None, application=None):
        self._config = config_manager
        app = application or QApplication.instance()
        self._application = app if isinstance(app, QApplication) else None
        saved = (
            config_manager.get_app_setting("ui_theme_mode", "system")
            if config_manager is not None
            else "system"
        )
        self._mode = UIThemeMode.coerce(saved)
        self._connect_system_theme()
        self.apply(force=True)
        return self

    @property
    def mode(self) -> UIThemeMode:
        return self._mode

    @property
    def effective_mode(self) -> UIThemeMode:
        return self._effective_mode

    @property
    def is_dark(self) -> bool:
        return self._effective_mode is UIThemeMode.DARK

    @property
    def tokens(self) -> UIThemeTokens:
        return DARK_TOKENS if self.is_dark else LIGHT_TOKENS

    def set_mode(self, mode, persist: bool = True):
        normalized = UIThemeMode.coerce(mode)
        changed = normalized is not self._mode
        self._mode = normalized
        if persist and self._config is not None:
            self._config.set_app_setting("ui_theme_mode", normalized.value)
        self.apply(force=changed)
        if changed:
            self.mode_changed.emit(normalized.value)

    def apply(self, force: bool = False):
        effective = self._resolve_effective_mode()
        changed = effective is not self._effective_mode
        self._effective_mode = effective
        if not (force or changed):
            return

        if self._application is not None:
            self._application.setProperty("uiTheme", effective.value)
            self._application.setPalette(self.build_palette(self.tokens))
            self._application.setStyleSheet(
                self._build_global_stylesheet(self.tokens)
            )

        self.theme_changed.emit(self.tokens)

    def _resolve_effective_mode(self) -> UIThemeMode:
        if self._mode is not UIThemeMode.SYSTEM:
            return self._mode
        hints = QGuiApplication.styleHints()
        if hints is not None and hasattr(hints, "colorScheme"):
            scheme = hints.colorScheme()
            if scheme == Qt.ColorScheme.Dark:
                return UIThemeMode.DARK
            if scheme == Qt.ColorScheme.Light:
                return UIThemeMode.LIGHT

        # Some remote-desktop and older platform plugins report Unknown.
        app = self._application or QApplication.instance()
        if app is not None:
            window = app.palette().color(QPalette.ColorRole.Window)
            return (
                UIThemeMode.DARK
                if window.lightness() < 128
                else UIThemeMode.LIGHT
            )
        return UIThemeMode.LIGHT

    def _connect_system_theme(self):
        if self._system_signal_connected:
            return
        hints = QGuiApplication.styleHints()
        signal = getattr(hints, "colorSchemeChanged", None) if hints else None
        if signal is not None:
            signal.connect(self._on_system_theme_changed)
            self._system_signal_connected = True

    def _on_system_theme_changed(self, _scheme):
        if self._mode is UIThemeMode.SYSTEM:
            self.apply()

    @staticmethod
    def build_palette(t: UIThemeTokens) -> QPalette:
        palette = QPalette()
        normal = QPalette.ColorGroup.Normal
        inactive = QPalette.ColorGroup.Inactive

        roles = {
            QPalette.ColorRole.Window: t.window,
            QPalette.ColorRole.WindowText: t.text,
            QPalette.ColorRole.Base: t.input_background,
            QPalette.ColorRole.AlternateBase: t.surface_subtle,
            QPalette.ColorRole.ToolTipBase: t.popup_background,
            QPalette.ColorRole.ToolTipText: t.text,
            QPalette.ColorRole.Text: t.text,
            QPalette.ColorRole.Button: t.surface_strong,
            QPalette.ColorRole.ButtonText: t.text,
            QPalette.ColorRole.BrightText: "#FFFFFF",
            QPalette.ColorRole.Link: t.accent,
            QPalette.ColorRole.Highlight: t.accent,
            QPalette.ColorRole.HighlightedText: t.selected_text,
            QPalette.ColorRole.PlaceholderText: t.text_muted,
            QPalette.ColorRole.Mid: t.border_hover,
            QPalette.ColorRole.Dark: t.window_border,
        }
        for group in (normal, inactive):
            for role, value in roles.items():
                palette.setColor(group, role, QColor(value))

        disabled = QPalette.ColorGroup.Disabled
        for role, value in roles.items():
            palette.setColor(disabled, role, QColor(value))
        for role in (
            QPalette.ColorRole.WindowText,
            QPalette.ColorRole.Text,
            QPalette.ColorRole.ButtonText,
            QPalette.ColorRole.PlaceholderText,
        ):
            palette.setColor(disabled, role, QColor(t.text_disabled))
        return palette

    @staticmethod
    def _build_global_stylesheet(t: UIThemeTokens) -> str:
        # Keep this intentionally small.  Feature windows with their own theme
        # (for example Clipboard) remain free to override it locally.
        return f"""
            QToolTip {{
                color: {t.text};
                background-color: {t.popup_background};
                border: 1px solid {t.border_hover};
                padding: 4px 7px;
            }}
            QMenu {{
                color: {t.text};
                background-color: {t.popup_background};
                border: 1px solid {t.border};
                border-radius: 6px;
                padding: 4px;
            }}
            QMenu::item {{
                color: {t.text};
                background-color: transparent;
                padding: 6px 28px 6px 26px;
                margin: 1px 0;
            }}
            QMenu::item:selected {{
                color: {t.text};
                background-color: {t.popup_hover};
            }}
            QMenu::item:disabled {{
                color: {t.text_disabled};
                background-color: transparent;
            }}
            QMenu::separator {{
                height: 1px;
                background: {t.separator};
                margin: 4px 7px;
            }}
        """


_manager: Optional[UIThemeManager] = None


def get_ui_theme() -> UIThemeManager:
    global _manager
    if _manager is None:
        _manager = UIThemeManager()
    return _manager
