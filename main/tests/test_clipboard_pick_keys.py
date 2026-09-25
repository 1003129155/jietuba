# -*- coding: utf-8 -*-
"""剪贴板直选粘贴键：三套键位，以及和剪贴板快捷键共用一套判重。"""

from types import SimpleNamespace

import pytest
from PySide6.QtCore import Qt
from PySide6.QtWidgets import QApplication

from clipboard.ui.widgets.item_delegate import ClipboardItemDelegate
from clipboard.ui.windows.clipboard_window import ClipboardShortcutHandler
from settings import get_tool_settings_manager
from settings.tool_settings import ToolSettingsManager
from ui.settings_ui import page_hotkey


class _Event:
    def __init__(self, key, modifiers=Qt.KeyboardModifier.NoModifier):
        self._key, self._modifiers = key, modifiers

    def key(self):
        return self._key

    def modifiers(self):
        return self._modifiers


@pytest.fixture
def pick_mode():
    config = get_tool_settings_manager()

    def _set(mode):
        config.set_inapp_clipboard_pick_mode(mode)

    yield _set
    config.set_inapp_clipboard_pick_mode("both")


def _handler(monkeypatch):
    monkeypatch.setattr(QApplication, "focusWidget", staticmethod(lambda: None))
    pasted = []
    window = SimpleNamespace(
        search_input=object(),
        selection_manager=SimpleNamespace(_selected_index=-1),
        controller=SimpleNamespace(current_items=[SimpleNamespace(id=100 + i) for i in range(40)]),
        _on_paste_item=pasted.append,
        _edit_selected=lambda: False,
    )
    return ClipboardShortcutHandler(window), pasted


class TestHandler:
    @pytest.mark.parametrize(
        "mode, key, expected",
        [
            ("both", Qt.Key.Key_1, 100),
            ("both", Qt.Key.Key_A, 109),
            ("letters", Qt.Key.Key_A, 100),
            ("letters", Qt.Key.Key_1, None),
            ("digits", Qt.Key.Key_9, 108),
            ("digits", Qt.Key.Key_A, None),
        ],
    )
    def test_mode_decides_which_keys_pick_which_row(self, monkeypatch, pick_mode, mode, key, expected):
        pick_mode(mode)
        handler, pasted = _handler(monkeypatch)

        assert handler.handle_key(_Event(key)) is (expected is not None)
        assert pasted == ([expected] if expected is not None else [])

    def test_other_keys_do_not_pick_the_first_row(self, monkeypatch):
        handler, pasted = _handler(monkeypatch)

        assert handler.handle_key(_Event(Qt.Key.Key_F5)) is False
        assert pasted == []


def test_row_badges_follow_the_mode(qapp):
    delegate = ClipboardItemDelegate()

    delegate.set_pick_keys("abc")

    assert delegate._shortcut_labels == ["a：", "b：", "c："]


@pytest.mark.parametrize(
    "text, char",
    [("e", "e"), ("shift+e", "e"), ("3", "3"), ("ctrl+e", ""), ("tab", ""), ("0", ""), ("f2", "")],
)
def test_pick_char_of(text, char):
    assert page_hotkey._pick_char_of(text) == char


class TestSettingsConflict:
    @pytest.fixture
    def page(self, qapp, tmp_settings, monkeypatch):
        manager = ToolSettingsManager(qsettings=tmp_settings)
        dialog = SimpleNamespace(
            config_manager=manager,
            current_hotkey=manager.get_hotkey(),
            _get_input_style=lambda: "",
            tr=lambda text: text,
        )
        widget = page_hotkey.create_hotkey_page(dialog)
        dialog.asked = []
        dialog.answer = True
        monkeypatch.setattr(
            page_hotkey, "show_confirm_dialog",
            lambda *args, **kwargs: dialog.asked.append(args) or dialog.answer,
        )
        yield dialog
        widget.close()

    def test_taking_a_pick_letter_switches_picks_to_digits(self, page):
        page._inapp_edits["inapp_clipboard_quick_edit"].setText("e")

        assert len(page.asked) == 1
        assert page.clipboard_pick_combo.currentData() == "digits"
        assert page._inapp_edits["inapp_clipboard_quick_edit"].text() == "e"

    def test_declining_restores_the_shortcut(self, page):
        page.answer = False

        page._inapp_edits["inapp_clipboard_quick_edit"].setText("3")

        assert page.clipboard_pick_combo.currentData() == "both"
        assert page._inapp_edits["inapp_clipboard_quick_edit"].text() == "tab"

    def test_key_outside_the_current_pick_set_is_free(self, page):
        page.clipboard_pick_combo.setCurrentIndex(page.clipboard_pick_combo.findData("digits"))

        page._inapp_edits["inapp_clipboard_quick_edit"].setText("e")

        assert page.asked == []

    def test_switching_picks_onto_a_used_key_clears_it(self, page):
        page.clipboard_pick_combo.setCurrentIndex(page.clipboard_pick_combo.findData("digits"))
        page._inapp_edits["inapp_clipboard_quick_edit"].setText("e")

        page.clipboard_pick_combo.setCurrentIndex(page.clipboard_pick_combo.findData("letters"))

        assert len(page.asked) == 1
        assert page._inapp_edits["inapp_clipboard_quick_edit"].text() == ""
        assert page.clipboard_pick_combo.currentData() == "letters"

    def test_declining_the_switch_keeps_the_old_picks(self, page):
        page.clipboard_pick_combo.setCurrentIndex(page.clipboard_pick_combo.findData("digits"))
        page._inapp_edits["inapp_clipboard_quick_edit"].setText("e")
        page.answer = False

        page.clipboard_pick_combo.setCurrentIndex(page.clipboard_pick_combo.findData("both"))

        assert page.clipboard_pick_combo.currentData() == "digits"
        assert page._inapp_edits["inapp_clipboard_quick_edit"].text() == "e"

    def test_other_groups_ignore_pick_keys(self, page):
        page._inapp_edits["inapp_thumbnail"].setText("q")

        assert page.asked == []
