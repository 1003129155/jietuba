# -*- coding: utf-8 -*-
"""文本条目的快速编辑：入口规则、按键、保存与粘贴。"""

from types import SimpleNamespace

import pytest
from PySide6.QtCore import QPoint, QRect, Qt
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QListWidget, QListWidgetItem, QWidget

from clipboard.controllers.clipboard_controller import ClipboardController
from clipboard.controllers.context_menu_controller import is_quick_editable
from clipboard.core import ClipboardItem
from clipboard.ui.theme.themes import THEME_LIGHT
from clipboard.ui.widgets.item_delegate import ROLE_ITEM_DATA, ROLE_ITEM_ID
from clipboard.ui.widgets.quick_edit_popup import SAVE_SHORTCUT, QuickEditPopup, QuickEditResult
from clipboard.ui.windows.clipboard_window import QUICK_EDIT_SHORTCUT, ClipboardShortcutHandler, ClipboardWindow
from ui.inapp_key_edit import InAppKeyEdit


def _text(item_id=1, content="旧内容", **kwargs):
    return ClipboardItem(id=item_id, content=content, content_type="text", **kwargs)


class TestEntryRule:
    @pytest.mark.parametrize("item", [_text(), _text(title="标题")], ids=["untitled", "titled"])
    def test_text_is_quick_editable(self, item):
        assert is_quick_editable(item) is True

    @pytest.mark.parametrize(
        "item",
        [
            ClipboardItem(id=1, content="[10x10]", content_type="image"),
            ClipboardItem(id=1, content='{"files": []}', content_type="file"),
        ],
        ids=["image", "file"],
    )
    def test_images_and_files_are_not(self, item):
        assert is_quick_editable(item) is False

    def test_history_menu_offers_quick_edit_instead_of_edit(self, monkeypatch):
        monkeypatch.setattr(ClipboardController, "_load_settings", lambda self: None)
        manager = SimpleNamespace(get_item=lambda _id: _text(), get_groups=lambda: [])
        controller = ClipboardController(manager)

        keys = [action.key for action in controller.build_context_menu_data(1).actions]

        assert "quick_edit_item" in keys
        assert "edit_item" not in keys


class _Manager:
    def __init__(self, items):
        self.items = {item.id: item for item in items}
        self.updates = []
        self.update_ok = True

    def get_item(self, item_id):
        return self.items.get(item_id)

    def update_item(self, item_id, content, title=None):
        self.updates.append((item_id, content, title))
        if self.update_ok:
            old = self.items[item_id]
            self.items[item_id] = ClipboardItem(id=item_id, content=content, content_type=old.content_type, title=title)
        return self.update_ok


class TestControllerUpdate:
    @pytest.fixture
    def controller(self, monkeypatch):
        monkeypatch.setattr(ClipboardController, "_load_settings", lambda self: None)
        ctrl = ClipboardController(_Manager([_text(1), _text(2, title="保留的标题")]))
        ctrl.current_items = [_text(9, "别的"), _text(1), _text(2, title="保留的标题")]
        ctrl.updated = []
        ctrl.item_updated.connect(lambda row, item: ctrl.updated.append((row, item.content)))
        return ctrl

    def test_updates_the_loaded_row_in_place(self, controller):
        assert controller.update_item_content(1, "新内容") is True

        assert controller.updated == [(1, "新内容")]
        assert controller.current_items[1].content == "新内容"

    def test_keeps_the_existing_title(self, controller):
        controller.update_item_content(2, "新内容")

        assert controller.manager.updates == [(2, "新内容", "保留的标题")]

    def test_failed_update_leaves_the_row_alone(self, controller):
        controller.manager.update_ok = False

        assert controller.update_item_content(1, "新内容") is False
        assert controller.updated == []
        assert controller.current_items[1].content == "旧内容"

    def test_missing_item_is_not_updated(self, controller):
        assert controller.update_item_content(404, "x") is False
        assert controller.manager.updates == []


class TestPopupKeys:
    @pytest.fixture
    def popup(self, qapp):
        parent = QWidget()
        widget = QuickEditPopup(parent)
        widget.results = []
        widget.finished.connect(widget.results.append)
        widget.open_for(_text(7, "abc"), QPoint(100, 100), QRect(0, 0, 50, 50), THEME_LIGHT)
        yield widget
        parent.deleteLater()

    def test_ctrl_enter_saves(self, popup):
        QTest.keyClicks(popup.editor, "d")
        QTest.keyClick(popup.editor, Qt.Key.Key_Return, Qt.KeyboardModifier.ControlModifier)

        assert popup.results == [QuickEditResult(7, "abcd")]
        assert popup.is_open is False

    def test_ctrl_shift_enter_saves_and_pastes(self, popup):
        modifiers = Qt.KeyboardModifier.ControlModifier | Qt.KeyboardModifier.ShiftModifier
        QTest.keyClick(popup.editor, Qt.Key.Key_Return, modifiers)

        assert popup.results == [QuickEditResult(7, "abc", paste=True)]

    def test_plain_enter_inserts_a_line_break(self, popup):
        QTest.keyClick(popup.editor, Qt.Key.Key_Return)

        assert popup.results == []
        assert popup.editor.toPlainText() == "abc\n"

    def test_escape_cancels(self, popup):
        QTest.keyClick(popup.editor, Qt.Key.Key_Escape)

        assert popup.results == [QuickEditResult(7, None)]

    def test_focus_loss_saves_once(self, popup, monkeypatch):
        monkeypatch.setattr(popup.editor, "hasFocus", lambda: False)

        popup._on_editor_focus_out()
        popup._on_editor_focus_out()

        assert popup.results == [QuickEditResult(7, "abc", focus_left=True)]

    def test_focus_back_in_the_editor_keeps_it_open(self, popup, monkeypatch):
        monkeypatch.setattr(popup.editor, "hasFocus", lambda: True)

        popup._on_editor_focus_out()

        assert popup.results == [] and popup.is_open

    def test_rebound_save_key_takes_effect_on_next_open(self, popup):
        from settings import get_tool_settings_manager

        config = get_tool_settings_manager()
        config.set_inapp_shortcut(SAVE_SHORTCUT, "ctrl+s")
        try:
            popup.cancel()
            popup.results.clear()
            popup.open_for(_text(7, "abc"), QPoint(0, 0), QRect(0, 0, 50, 50), THEME_LIGHT)

            QTest.keyClick(popup.editor, Qt.Key.Key_Return, Qt.KeyboardModifier.ControlModifier)
            assert popup.results == []
            QTest.keyClick(popup.editor, Qt.Key.Key_S, Qt.KeyboardModifier.ControlModifier)
            assert popup.results == [QuickEditResult(7, "abc")]
            assert popup.save_button.toolTip() == "Ctrl + S"
        finally:
            config.set_inapp_shortcut(SAVE_SHORTCUT, "ctrl+enter")

    def test_rich_text_warns_that_formatting_goes(self, popup):
        assert popup.format_hint.text() == ""

        popup.cancel()
        popup.open_for(_text(8, "x", html_content="<b>x</b>"), QPoint(0, 0), QRect(0, 0, 50, 50), THEME_LIGHT)

        assert popup.format_hint.text() != ""


class _Window:
    """_on_quick_edit_finished 只用到的那些成员。"""

    def __init__(self, save_ok=True):
        self.calls = []
        self._save_ok = save_ok
        self.list_widget = SimpleNamespace(setFocus=lambda: self.calls.append("focus"))

    def isVisible(self):
        return True

    def _save_quick_edit(self, item_id, text):
        self.calls.append(("save", item_id, text))
        return self._save_ok

    def _paste_item_to_clipboard(self, item_id):
        self.calls.append(("paste", item_id))

    def activateWindow(self):
        self.calls.append("activate")

    def _check_and_hide(self):
        self.calls.append("check_hide")


class TestWindowFinish:
    def test_save_and_paste(self, qapp):
        window = _Window()

        ClipboardWindow._on_quick_edit_finished(window, QuickEditResult(5, "新", paste=True))

        assert window.calls == [("save", 5, "新"), ("paste", 5)]

    def test_failed_save_does_not_paste_stale_content(self, qapp):
        window = _Window(save_ok=False)

        ClipboardWindow._on_quick_edit_finished(window, QuickEditResult(5, "新", paste=True))

        assert ("paste", 5) not in window.calls

    def test_cancel_saves_nothing_and_returns_to_the_list(self, qapp):
        window = _Window()

        ClipboardWindow._on_quick_edit_finished(window, QuickEditResult(5, None))

        assert window.calls == ["activate", "focus"]

    def test_focus_loss_does_not_steal_focus_back(self, qapp, monkeypatch):
        window = _Window()
        monkeypatch.setattr("clipboard.ui.windows.clipboard_window.QTimer.singleShot", lambda _ms, fn: fn())

        ClipboardWindow._on_quick_edit_finished(window, QuickEditResult(5, "新", focus_left=True))

        assert window.calls == [("save", 5, "新"), "check_hide"]

    def test_unchanged_text_is_not_written(self):
        window = SimpleNamespace(
            _get_item_data=lambda _id: _text(5, "同样"),
            controller=SimpleNamespace(update_item_content=pytest.fail),
        )

        assert ClipboardWindow._save_quick_edit(window, 5, "同样") is True


class TestPasteTarget:
    """编辑期间编辑框在前台；记成粘贴目标的话，保存后它一隐藏，粘贴就找不到窗口。"""

    @pytest.fixture
    def picker(self, qapp):
        window = QWidget()
        window._owns_window = lambda candidate: ClipboardWindow._owns_window(window, candidate)
        yield window
        window.deleteLater()

    def test_quick_edit_popup_is_not_a_paste_target(self, picker):
        popup = QuickEditPopup(picker)

        assert ClipboardWindow._is_paste_picker_surface(picker, int(popup.winId())) is True

    def test_unrelated_window_of_this_app_still_is(self, picker):
        other = QWidget()

        assert ClipboardWindow._is_paste_picker_surface(picker, int(other.winId())) is False
        other.deleteLater()


class TestWindowRowRefresh:
    def test_updated_item_replaces_the_row_data(self, qapp):
        widget = QListWidget()
        for item_id in (1, 2):
            row = QListWidgetItem()
            row.setData(ROLE_ITEM_ID, item_id)
            row.setData(ROLE_ITEM_DATA, _text(item_id))
            widget.addItem(row)

        ClipboardWindow._on_item_updated(SimpleNamespace(list_widget=widget), 1, _text(2, "新内容"))

        assert widget.item(1).data(ROLE_ITEM_DATA).content == "新内容"
        assert widget.item(0).data(ROLE_ITEM_DATA).content == "旧内容"
        widget.deleteLater()


class _Event:
    def __init__(self, key, modifiers=Qt.KeyboardModifier.NoModifier):
        self._key, self._modifiers = key, modifiers

    def key(self):
        return self._key

    def modifiers(self):
        return self._modifiers


class TestShortcuts:
    def _window(self, popup_open=False):
        edits = []
        return SimpleNamespace(
            quick_edit_popup=SimpleNamespace(is_open=popup_open),
            selection_manager=SimpleNamespace(_selected_index=0, reset=lambda: edits.append("reset")),
            _edit_selected=lambda: edits.append("edit") or True,
            close=lambda: edits.append("close"),
            edits=edits,
        )

    @pytest.mark.parametrize(
        "item, group_id, expected, handled",
        [
            (_text(), None, "quick", True),
            (_text(title="分组标题"), 3, "quick", True),
            (ClipboardItem(id=1, content='{"files": []}', content_type="file"), 3, "dialog", True),
            (ClipboardItem(id=1, content='{"files": []}', content_type="file"), None, None, False),
        ],
        ids=["history-text", "group-text", "group-file", "history-file"],
    )
    def test_edit_selected_picks_the_editor(self, item, group_id, expected, handled):
        opened = []
        window = SimpleNamespace(
            selection_manager=SimpleNamespace(get_current_item_id=lambda: item.id),
            controller=SimpleNamespace(current_group_id=group_id),
            _get_item_data=lambda _id: item,
            _quick_edit_item=lambda _id: opened.append("quick"),
            _edit_item=lambda _id: opened.append("dialog"),
        )

        assert ClipboardWindow._edit_selected(window) is handled
        assert opened == ([expected] if expected else [])

    def test_tab_edits_the_selected_item(self):
        window = self._window()

        assert ClipboardShortcutHandler(window).handle_key(_Event(Qt.Key.Key_Tab)) is True
        assert window.edits == ["edit"]

    def test_keys_go_to_the_open_editor(self):
        window = self._window(popup_open=True)
        handler = ClipboardShortcutHandler(window)

        assert handler.handle_key(_Event(Qt.Key.Key_Escape)) is False
        assert handler.handle_key(_Event(Qt.Key.Key_Tab)) is False
        assert window.edits == []

    def test_rebound_key_takes_effect_after_reload(self):
        from settings import get_tool_settings_manager

        config = get_tool_settings_manager()
        window = self._window()
        handler = ClipboardShortcutHandler(window)
        config.set_inapp_shortcut(QUICK_EDIT_SHORTCUT, "ctrl+e")
        try:
            handler.reload_bindings()

            assert handler.handle_key(_Event(Qt.Key.Key_Tab)) is False
            assert handler.handle_key(_Event(Qt.Key.Key_E, Qt.KeyboardModifier.ControlModifier)) is True
            assert window.edits == ["edit"]
        finally:
            config.set_inapp_shortcut(QUICK_EDIT_SHORTCUT, "tab")

    def test_nothing_to_edit_lets_the_key_through(self):
        window = self._window()
        window._edit_selected = lambda: False

        assert ClipboardShortcutHandler(window).handle_key(_Event(Qt.Key.Key_Tab)) is False


class TestKeyEdit:
    def test_tab_is_recorded_instead_of_moving_focus(self, qapp):
        edit = InAppKeyEdit()

        QTest.keyClick(edit, Qt.Key.Key_Tab)

        assert edit.text() == "tab"
        edit.deleteLater()

    @pytest.mark.parametrize(
        "key, modifiers",
        [
            (Qt.Key.Key_A, Qt.KeyboardModifier.NoModifier),
            (Qt.Key.Key_Space, Qt.KeyboardModifier.NoModifier),
            (Qt.Key.Key_Left, Qt.KeyboardModifier.NoModifier),
            (Qt.Key.Key_Delete, Qt.KeyboardModifier.NoModifier),
        ],
        ids=["letter", "space", "arrow", "delete"],
    )
    def test_editor_keys_reject_typing_and_cursor_keys(self, qapp, key, modifiers):
        edit = InAppKeyEdit(allow_text_keys=False)
        edit.setText("ctrl+enter")

        QTest.keyClick(edit, key, modifiers)

        assert edit.text() == "ctrl+enter"
        edit.deleteLater()

    def test_editor_keys_reject_shifted_letters(self, qapp):
        edit = InAppKeyEdit(allow_text_keys=False)

        # QTest 会连同 Shift 的按下、松开一起发；只按修饰键就松开会清空，和真人操作一致
        QTest.keyClick(edit, Qt.Key.Key_A, Qt.KeyboardModifier.ShiftModifier)

        assert edit.text() != "shift+a"
        edit.deleteLater()

    @pytest.mark.parametrize(
        "key, modifiers, expected",
        [
            (Qt.Key.Key_S, Qt.KeyboardModifier.ControlModifier, "ctrl+s"),
            (Qt.Key.Key_Return, Qt.KeyboardModifier.NoModifier, "enter"),
            (Qt.Key.Key_F2, Qt.KeyboardModifier.NoModifier, "f2"),
        ],
    )
    def test_editor_keys_accept_combinations_and_function_keys(self, qapp, key, modifiers, expected):
        edit = InAppKeyEdit(allow_text_keys=False)

        QTest.keyClick(edit, key, modifiers)

        assert edit.text() == expected
        edit.deleteLater()

    def test_middle_click_is_ignored_when_mouse_is_not_allowed(self, qapp):
        edit = InAppKeyEdit(allow_mouse=False)

        QTest.mouseClick(edit, Qt.MouseButton.MiddleButton)

        assert edit.text() == ""
        edit.deleteLater()
