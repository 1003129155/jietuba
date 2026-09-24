# -*- coding: utf-8 -*-
"""快捷键按键块：显示文字、× 清除和状态图标。"""
import pytest
from PySide6.QtCore import QPoint, Qt
from PySide6.QtTest import QTest

from ui.key_chip import CHIP_HEIGHT, CHIP_WIDTH, format_shortcut_text


@pytest.mark.parametrize(
    ("value", "expected"),
    [
        ("", ""),
        ("ctrl+shift+a", "Ctrl + Shift + A"),
        ("f3", "F3"),
        ("pageup", "PageUp"),
        ("escape", "Esc"),
        ("ctrl+", "Ctrl +"),
        ("ctrl++", "Ctrl + +"),
        ("mouseforward", "Mouse Forward"),
    ],
)
def test_config_value_is_shown_as_readable_keys(value, expected):
    assert format_shortcut_text(value) == expected


def _sized_chip(qapp):
    from core.ui_scale import dialog_scaled
    from ui.inapp_key_edit import InAppKeyEdit

    chip = InAppKeyEdit()
    chip.resize(dialog_scaled(CHIP_WIDTH), dialog_scaled(CHIP_HEIGHT))
    chip.setText("ctrl+c")
    return chip


def test_clicking_the_cross_clears_the_binding(qapp):
    chip = _sized_chip(qapp)
    QTest.mouseClick(
        chip, Qt.MouseButton.LeftButton, pos=QPoint(chip.width() - chip.height() // 2, chip.height() // 2)
    )
    assert chip.text() == ""


def test_clicking_the_key_text_keeps_the_binding(qapp):
    chip = _sized_chip(qapp)
    QTest.mouseClick(chip, Qt.MouseButton.LeftButton, pos=QPoint(10, chip.height() // 2))
    assert chip.text() == "ctrl+c"


def test_hotkey_edit_marks_empty_slot_as_idle_and_conflict_as_error(qapp):
    from ui.hotkey_edit import HotkeyEdit

    widget = HotkeyEdit()
    assert widget.status_state == "idle"
    assert widget.edit.errorState() is False

    widget.set_validation_error("duplicated")
    assert widget.status_state == "error"
    assert widget.edit.errorState() is True
    assert widget.status_icon.toolTip() == "duplicated"
