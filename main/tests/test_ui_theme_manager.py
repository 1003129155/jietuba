# -*- coding: utf-8 -*-
"""Tests for the application light/dark appearance manager."""

import pytest

from PySide6.QtGui import QColor, QPalette
from PySide6.QtWidgets import QWidget

from core.ui_theme import (
    DARK_TOKENS,
    LIGHT_TOKENS,
    UIThemeManager,
    UIThemeMode,
)


class _Config:
    def __init__(self, mode="system"):
        self.values = {"ui_theme_mode": mode}

    def get_app_setting(self, key, default=None):
        return self.values.get(key, default)

    def set_app_setting(self, key, value):
        self.values[key] = value


@pytest.fixture(autouse=True)
def _restore_application_theme(qapp):
    old_palette = qapp.palette()
    old_stylesheet = qapp.styleSheet()
    yield
    qapp.setPalette(old_palette)
    qapp.setStyleSheet(old_stylesheet)


def test_explicit_light_and_dark_modes_update_palette(qapp):
    config = _Config("light")
    manager = UIThemeManager()
    manager.init(config, qapp)
    assert manager.mode is UIThemeMode.LIGHT
    assert manager.tokens is LIGHT_TOKENS
    assert qapp.palette().color(QPalette.ColorRole.WindowText) == QColor(
        LIGHT_TOKENS.text
    )
    assert f"background-color: {LIGHT_TOKENS.popup_background}" in qapp.styleSheet()
    assert f"color: {LIGHT_TOKENS.text_disabled}" in qapp.styleSheet()

    manager.set_mode("dark")
    assert manager.mode is UIThemeMode.DARK
    assert manager.tokens is DARK_TOKENS
    assert config.values["ui_theme_mode"] == "dark"
    assert qapp.palette().color(QPalette.ColorRole.WindowText) == QColor(
        DARK_TOKENS.text
    )
    assert f"background-color: {DARK_TOKENS.popup_background}" in qapp.styleSheet()
    assert f"color: {DARK_TOKENS.text_disabled}" in qapp.styleSheet()


def test_invalid_mode_falls_back_to_system(qapp):
    manager = UIThemeManager()
    manager.init(_Config("not-a-theme"), qapp)
    assert manager.mode is UIThemeMode.SYSTEM


def test_enum_mode_is_preserved(qapp):
    manager = UIThemeManager()
    manager.init(_Config("light"), qapp)
    manager.set_mode(UIThemeMode.DARK)
    assert manager.mode is UIThemeMode.DARK
    assert manager.effective_mode is UIThemeMode.DARK


def test_mode_change_can_be_previewed_without_persisting(qapp):
    config = _Config("light")
    manager = UIThemeManager()
    manager.init(config, qapp)
    manager.set_mode("dark", persist=False)
    assert manager.is_dark
    assert config.values["ui_theme_mode"] == "light"


def test_widget_theme_scope_can_opt_out_of_application_theme(qapp):
    from ui.fluent_lite import ComboBox, PushButton

    root = QWidget()
    root._ui_theme_tokens_override = LIGHT_TOKENS
    button = PushButton("Button", root)
    combo = ComboBox(root)

    assert LIGHT_TOKENS.text in button.styleSheet()
    assert LIGHT_TOKENS.input_background in combo.styleSheet()

    root.deleteLater()
