# -*- coding: utf-8 -*-
"""全局热键的键名表、注册方式判定，以及录入框、旧设置迁移如何用它"""
import pytest
from PySide6.QtCore import QEvent, Qt
from PySide6.QtGui import QFocusEvent, QKeyEvent

from core import global_hotkey
from core.global_hotkey import (
    HOOK, INVALID, MOD_ALT, MOD_CONTROL, MOD_SHIFT, MOD_WIN, MOUSE, REGISTER, RESERVED, UNSET,
    VK_SNAPSHOT, format_hotkey, parse_hotkey, route,
)
from core.input_hub import input_hub
from core.shortcut_manager import ShortcutManager, get_key_display_map
from settings.tool_settings import ToolSettingsManager

VK_LWIN, VK_V = 0x5B, ord("V")


class TestRoute:
    @pytest.mark.parametrize("text, expected", [
        ("", UNSET),
        ("   ", UNSET),
        ("mouseback", MOUSE),
        ("ctrl+1", REGISTER),
        ("Ctrl + Shift + A", REGISTER),
        ("shift+a", REGISTER),
        ("ctrl+space", REGISTER),
        ("f5", REGISTER),
        ("home", REGISTER),
        ("left", REGISTER),
        ("pause", REGISTER),
        ("win+v", HOOK),
        ("shift+win+s", HOOK),
        ("win+1", HOOK),
        ("print", HOOK),
        ("prtsc", HOOK),
        ("ctrl+print", HOOK),
        ("win+l", RESERVED),
        ("shift+win+l", RESERVED),
        ("ctrl+alt+delete", RESERVED),
        ("ctrl+alt+shift+del", RESERVED),
        ("ctrl+delete", REGISTER),
        ("a", INVALID),
        ("esc", INVALID),
        ("space", INVALID),
        ("ctrl+", INVALID),
        ("ctrl+!", INVALID),
        ("ctrl+a+b", INVALID),
        ("ctrl", INVALID),
        ("mousemiddle", INVALID),
    ])
    def test_route(self, text, expected):
        assert route(text) == expected

    def test_every_key_the_edit_can_write_parses(self):
        names = [name.lower() for name in get_key_display_map().values()]
        names += [chr(c) for c in range(ord("a"), ord("z") + 1)]
        names += [str(n) for n in range(10)] + [f"f{n}" for n in range(1, 25)]
        names += list("`-=[]\\;',./")
        for name in names:
            parse_hotkey(f"ctrl+{name}")

    def test_written_names_read_back_to_the_same_key(self):
        for name, vk in global_hotkey._NAME_TO_VK.items():
            mods = MOD_CONTROL | MOD_SHIFT | MOD_ALT | MOD_WIN
            assert parse_hotkey(format_hotkey(mods, vk)) == (mods, vk), name

    def test_modifiers_are_written_in_the_edits_order(self):
        assert format_hotkey(MOD_WIN | MOD_ALT | MOD_SHIFT | MOD_CONTROL, ord("X")) == "ctrl+shift+alt+win+x"

    def test_unknown_virtual_keys_have_no_name(self):
        assert format_hotkey(MOD_WIN, 0xFF) is None


@pytest.fixture
def edit(qapp, monkeypatch):
    from ui.hotkey_edit import HotkeyEdit

    probes = {}
    monkeypatch.setattr(
        "core.shortcut_manager.HotkeySystem.check_hotkey_availability",
        lambda _self, hotkey: probes.get(hotkey, True),
    )
    widget = HotkeyEdit()
    widget.probes = probes
    yield widget
    widget.deleteLater()


class TestHotkeyEditStatus:
    def test_a_combination_windows_holds_is_taken_over(self, edit):
        edit.probes["win+v"] = False
        edit.setText("win+v")
        assert edit.status_state == "ok"
        assert edit.taken_over
        assert "clear it" in edit.status_icon.toolTip()

    def test_a_free_win_combination_is_just_available(self, edit):
        edit.setText("win+j")
        assert edit.status_state == "ok"
        assert not edit.taken_over

    def test_print_screen_is_taken_over_even_when_it_registers(self, edit):
        edit.setText("print")
        assert edit.taken_over

    @pytest.mark.parametrize("text, message", [
        ("win+l", "Reserved by Windows"),
        ("ctrl+!", "can't be used"),
    ])
    def test_values_no_one_can_bind_are_errors(self, edit, text, message):
        edit.setText(text)
        assert edit.status_state == "error"
        assert message in edit.status_icon.toolTip()
        assert edit.validate_now() is False

    def test_a_combination_another_app_holds_is_a_conflict(self, edit):
        edit.probes["ctrl+alt+a"] = False
        edit.setText("ctrl+alt+a")
        assert edit.status_state == "error"
        assert "another app" in edit.status_icon.toolTip()


def _key(key, modifiers=Qt.KeyboardModifier.NoModifier):
    return QKeyEvent(QEvent.Type.KeyPress, key, modifiers, "")


class TestHotkeyEditRecording:
    @pytest.mark.parametrize("key, expected", [
        (Qt.Key.Key_Home, "home"),
        (Qt.Key.Key_F5, "f5"),
        (Qt.Key.Key_Print, "print"),
    ])
    def test_keys_that_bind_alone_are_recorded(self, edit, key, expected):
        edit.edit.keyPressEvent(_key(key))
        assert edit.text() == expected

    @pytest.mark.parametrize("key", [Qt.Key.Key_A, Qt.Key.Key_Space, Qt.Key.Key_Escape])
    def test_typing_keys_alone_are_ignored(self, edit, key):
        edit.setText("ctrl+1")
        edit.edit.keyPressEvent(_key(key))
        assert edit.text() == "ctrl+1"

    def test_unknown_keys_leave_the_value_alone(self, edit):
        edit.setText("win+v")
        edit.edit.keyPressEvent(_key(Qt.Key.Key_unknown, Qt.KeyboardModifier.MetaModifier))
        assert edit.text() == "win+v"

    def test_combinations_are_recorded_even_when_they_cannot_bind(self, edit, qtbot):
        edit.edit.keyPressEvent(_key(Qt.Key.Key_L, Qt.KeyboardModifier.MetaModifier))
        assert edit.text() == "win+l"
        qtbot.waitUntil(lambda: edit.status_state == "error", timeout=2000)

    def test_a_registered_hotkey_pressed_while_recording_is_written_in(self, edit):
        mgr = ShortcutManager.instance()
        mgr._id_to_metadata[90001] = (MOD_CONTROL | MOD_ALT | global_hotkey.MOD_NOREPEAT, ord("A"))
        try:
            assert edit.edit._shortcut_handler.handle_hotkey(90001, lambda: None) is True
        finally:
            del mgr._id_to_metadata[90001]
        assert edit.text() == "ctrl+alt+a"

    def test_focus_captures_win_combinations_until_it_leaves(self, edit, qapp):
        native = input_hub().native
        edit.edit.focusInEvent(QFocusEvent(QEvent.Type.FocusIn))
        try:
            native.key(VK_LWIN, True)
            assert native.key(VK_V, True) is True
            native.key(VK_V, False)
            native.key(VK_LWIN, False)
            assert native.key(VK_SNAPSHOT, True) is True
            native.key(VK_SNAPSHOT, False)
            qapp.processEvents()
            assert edit.text() == "print"
        finally:
            edit.edit.focusOutEvent(QFocusEvent(QEvent.Type.FocusOut))
        native.key(VK_LWIN, True)
        assert native.key(VK_V, True) is False
        native.key(VK_V, False)
        native.key(VK_LWIN, False)


class TestWinVMigration:
    def _config(self, tmp_settings, **app_settings):
        config = ToolSettingsManager(qsettings=tmp_settings)
        for key, value in app_settings.items():
            config.set_app_setting(key, value)
        return config

    def test_new_installs_take_over_win_v_in_the_third_slot(self, tmp_settings):
        assert self._config(tmp_settings).get_global_hotkeys("clipboard") == ["ctrl+2", "", "win+v", ""]

    def test_the_old_setting_turned_off_leaves_it_empty(self, tmp_settings):
        config = self._config(tmp_settings, clipboard_take_over_win_v=False)
        assert config.get_global_hotkeys("clipboard")[2] == ""

    def test_reading_does_not_write_anything(self, tmp_settings):
        self._config(tmp_settings).get_global_hotkeys("clipboard")
        assert not tmp_settings.contains("clipboard/hotkey_3")

    @pytest.mark.parametrize("function, slot", [("clipboard", 0), ("screenshot", 1), ("clipboard", 3)])
    def test_win_v_already_in_another_slot_is_not_doubled(self, tmp_settings, function, slot):
        config = self._config(tmp_settings)
        values = config.get_global_hotkeys(function)
        values[slot] = "Win + V"
        if function == "clipboard":
            values[2] = ""
            # 只写别的槽，第 3 槽保持未存过
            path = "clipboard/hotkey" + (f"_{slot + 1}" if slot else "")
            tmp_settings.setValue(path, values[slot])
        else:
            config.set_global_hotkeys(function, values)
        assert config.get_global_hotkeys("clipboard")[2] == ""

    def test_once_saved_the_slot_wins_over_the_old_setting(self, tmp_settings):
        config = self._config(tmp_settings)
        config.set_global_hotkeys("clipboard", ["ctrl+2", "", "", ""])
        config.set_app_setting("clipboard_take_over_win_v", True)
        assert config.get_global_hotkeys("clipboard")[2] == ""

    @pytest.mark.parametrize("values, expected", [
        (["ctrl+2", "", "", ""], False),
        (["ctrl+2", "", "win+v", ""], True),
        (["win+v", "", "", ""], True),
    ])
    def test_saving_keeps_the_old_setting_in_step_for_older_versions(self, tmp_settings, values, expected):
        config = self._config(tmp_settings)
        config.set_global_hotkeys("clipboard", values)
        assert config.get_app_setting("clipboard_take_over_win_v") is expected

    def test_a_wrong_slot_count_is_rejected(self, tmp_settings):
        with pytest.raises(ValueError):
            self._config(tmp_settings).set_global_hotkeys("clipboard", ["ctrl+2"])

    def test_importing_old_settings_follows_the_same_rule(self, tmp_settings, tmp_path):
        from settings.settings_transfer import preview_config

        config = self._config(tmp_settings)
        preview = preview_config(
            config, {"app/clipboard_take_over_win_v": False}, str(tmp_path / "preview.ini")
        )
        assert preview.get_global_hotkeys("clipboard")[2] == ""


@pytest.fixture
def settings_dialog(qapp, tmp_path, monkeypatch):
    from PySide6.QtCore import QSettings
    from ui.settings_ui.dialog import SettingsDialog

    manager = ToolSettingsManager(
        qsettings=QSettings(str(tmp_path / "settings.ini"), QSettings.Format.IniFormat)
    )
    manager.set_log_dir(str(tmp_path))
    monkeypatch.setattr("settings.get_tool_settings_manager", lambda: manager)
    monkeypatch.setattr("ui.settings_ui.dialog.log_info", lambda *_a, **_k: None)
    monkeypatch.setattr("core.shortcut_manager.HotkeySystem.check_hotkey_availability", lambda _s, _h: True)
    dialog = SettingsDialog(manager, manager.get_hotkey())
    yield dialog, manager
    dialog._skip_unsaved_close_prompt = True
    dialog.close()
    dialog.deleteLater()
    qapp.processEvents()


def _slots(dialog, first_attr):
    from ui.settings_ui.page_hotkey import global_hotkey_edit_attrs

    return [getattr(dialog, attr) for attr in global_hotkey_edit_attrs(first_attr)]


class TestSettingsPage:
    def test_each_function_has_four_slots_and_win_v_shows_in_the_third(self, settings_dialog):
        dialog, _manager = settings_dialog
        assert [edit.text() for edit in _slots(dialog, "clipboard_hotkey_edit")] == ["ctrl+2", "", "win+v", ""]
        for first_attr in ("hotkey_input", "pin_clipboard_hotkey_edit", "translation_hotkey_edit"):
            assert len(_slots(dialog, first_attr)) == 4

    def test_the_first_two_slots_keep_their_column(self, settings_dialog):
        from PySide6.QtWidgets import QGridLayout

        dialog, _manager = settings_dialog
        edits = _slots(dialog, "hotkey_input")
        row = edits[0].parentWidget().layout()
        grid = next(row.itemAt(i).layout() for i in range(row.count())
                    if isinstance(row.itemAt(i).layout(), QGridLayout))
        positions = [grid.getItemPosition(grid.indexOf(edit))[:2] for edit in edits]
        assert positions == [(0, 0), (1, 0), (0, 1), (1, 1)]

    def test_apply_saves_every_slot_and_keeps_the_old_setting_in_step(self, settings_dialog):
        dialog, manager = settings_dialog
        dialog.clipboard_hotkey_edit_3.setText("")
        dialog.hotkey_input_4.setText("print")
        assert dialog.apply_settings()
        assert manager.get_global_hotkeys("clipboard") == ["ctrl+2", "", "", ""]
        assert manager.get_app_setting("clipboard_take_over_win_v") is False
        assert manager.get_global_hotkeys("screenshot") == ["ctrl+1", "", "", "print"]

    def test_changing_a_new_slot_is_an_unsaved_change(self, settings_dialog):
        dialog, _manager = settings_dialog
        assert not dialog._has_unsaved_changes()
        dialog.translation_hotkey_edit_4.setText("ctrl+alt+t")
        assert dialog._has_unsaved_changes()

    def test_a_duplicate_in_the_new_slots_blocks_apply(self, settings_dialog, monkeypatch):
        dialog, manager = settings_dialog
        warnings = []
        monkeypatch.setattr("ui.settings_ui.dialog.show_warning_dialog", lambda *args: warnings.append(args))
        dialog.hotkey_input_3.setText("win+v")
        assert dialog.apply_settings() is False
        assert warnings
        assert manager.get_global_hotkeys("screenshot")[2] == ""

    def test_reset_puts_win_v_back(self, settings_dialog):
        dialog, _manager = settings_dialog
        dialog.clipboard_hotkey_edit_3.setText("")
        dialog._reset_hotkey_page()
        assert dialog.clipboard_hotkey_edit_3.text() == "win+v"
