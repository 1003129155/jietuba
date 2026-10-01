"""Global mouse shortcuts persist and participate in normal settings workflows."""

from pathlib import Path
from types import SimpleNamespace

import pytest
from PySide6.QtCore import QSettings, QTranslator

from settings.tool_settings import QUICK_CAPTURE_ACTIONS, ToolSettingsManager, get_quick_capture_bindings
from ui.settings_ui.dialog import SettingsDialog
from ui.settings_ui.page_mouse import _create_global_section


@pytest.fixture
def config(tmp_path):
    manager = ToolSettingsManager(qsettings=QSettings(str(tmp_path / "settings.ini"), QSettings.IniFormat))
    manager.set_log_dir(str(tmp_path))
    manager.set_screenshot_save_path(str(tmp_path / "captures"))
    return manager


@pytest.fixture
def settings(qapp, config, monkeypatch):
    monkeypatch.setattr("ui.settings_ui.dialog.validate_global_hotkey_edits", lambda *_a, **_kw: True)
    dialog = SettingsDialog(config)
    dialog.build_all_pages()
    # Saving this isolated dialog must not change machine autostart or logging.
    for attr in ("log_toggle", "autostart_toggle", "language_combo"):
        delattr(dialog, attr)
    yield dialog
    dialog._skip_unsaved_close_prompt = True
    dialog.close()
    dialog.deleteLater()


def editor(dialog, action):
    return dialog._behavior_controls[f"quick_capture_{action}"]


def choose(combo, value):
    index = combo.findData(value)
    assert index >= 0
    combo.setCurrentIndex(index)


def win(button="left"):
    return frozenset({"win"}), button


def test_default_binds_win_left_drag_to_pin_and_leaves_other_rows_empty(settings, config):
    assert [key for key in settings._behavior_controls if key.startswith("quick_capture_")] == [
        f"quick_capture_{action}" for action, *_ in QUICK_CAPTURE_ACTIONS]
    for action, *_ in QUICK_CAPTURE_ACTIONS:
        assert editor(settings, action).currentData() == ("win+dragleft" if action == "pin" else "")
    assert get_quick_capture_bindings(config) == {win(): "pin"}


def test_every_mouse_button_can_be_chosen(settings):
    gesture = editor(settings, "copy").gesture
    assert [gesture.itemData(i) for i in range(gesture.count())] == [
        "", "dragleft", "dragmiddle", "dragright", "dragx1", "dragx2"]


def test_modifier_choices_need_one_or_two_keys_and_start_with_win(settings):
    modifiers = editor(settings, "copy").modifiers
    values = [modifiers.itemData(i) for i in range(modifiers.count())]
    assert values[:2] == ["", "win"]
    assert modifiers.itemText(0) == ""
    assert all(1 <= len(value.split("+")) <= 2 for value in values[1:])


def test_choosing_a_button_fills_in_win_and_clearing_either_side_clears_both(settings):
    row = editor(settings, "ocr")
    choose(row.gesture, "dragx1")
    assert row.currentData() == "win+dragx1"
    choose(row.modifiers, "ctrl+alt")
    assert row.currentData() == "ctrl+alt+dragx1"
    choose(row.gesture, "")
    assert (row.modifiers.currentData(), row.currentData()) == ("", "")
    choose(row.gesture, "dragmiddle")
    choose(row.modifiers, "")
    assert (row.gesture.currentData(), row.currentData()) == ("", "")


def test_modifier_without_a_button_is_not_saved(settings, config):
    choose(editor(settings, "copy").modifiers, "shift+win")
    assert editor(settings, "copy").currentData() == ""
    assert settings.apply_settings()
    assert get_quick_capture_bindings(config) == {win(): "pin"}


def test_apply_persists_every_button_and_it_reloads(settings, config):
    choose(editor(settings, "pin").gesture, "")
    for action, modifiers, gesture in (
        ("copy", "ctrl+alt", "dragx1"), ("edit", "shift+win", "dragright"),
        ("ocr", "alt", "dragmiddle"), ("translate", "win", "dragx2"), ("copy_pin", "ctrl", "dragleft"),
    ):
        choose(editor(settings, action).gesture, gesture)
        choose(editor(settings, action).modifiers, modifiers)
    assert settings._has_unsaved_changes()
    assert settings.apply_settings()
    assert not settings._has_unsaved_changes()
    expected = {
        (frozenset({"ctrl", "alt"}), "x1"): "copy", (frozenset({"shift", "win"}), "right"): "edit",
        (frozenset({"alt"}), "middle"): "ocr", win("x2"): "translate", (frozenset({"ctrl"}), "left"): "copy_pin",
    }
    assert get_quick_capture_bindings(config) == expected
    restored = ToolSettingsManager(qsettings=QSettings(config.qsettings.fileName(), QSettings.IniFormat))
    assert get_quick_capture_bindings(restored) == expected


def test_clearing_the_default_stays_cleared_after_reload(settings, config):
    choose(editor(settings, "pin").gesture, "")
    assert settings.apply_settings()
    restored = ToolSettingsManager(qsettings=QSettings(config.qsettings.fileName(), QSettings.IniFormat))
    assert get_quick_capture_bindings(restored) == {}


def test_same_gesture_on_two_rows_blocks_apply(settings, config, monkeypatch):
    warnings = []
    monkeypatch.setattr("ui.settings_ui.dialog.show_warning_dialog", lambda *args: warnings.append(args))
    choose(editor(settings, "copy").gesture, "dragleft")
    assert editor(settings, "copy").currentData() == "win+dragleft"
    assert not settings.apply_settings()
    assert ("Global Mouse Shortcuts · Win + Left Drag: "
            "Screenshot (Mouse Shortcuts) / Pin to Screen (Mouse Shortcuts)") in warnings[0][2]
    assert config.get_app_setting("quick_capture_copy") == ""


def test_same_modifiers_on_different_buttons_do_not_conflict(settings, config):
    choose(editor(settings, "copy").gesture, "dragright")
    assert settings.apply_settings()
    assert get_quick_capture_bindings(config) == {win(): "pin", win("right"): "copy"}


def test_refresh_uses_saved_value_and_reset_is_not_saved_implicitly(settings, config):
    config.set_app_setting("quick_capture_pin", "")
    config.set_app_setting("quick_capture_edit", "ctrl+win+dragx2")
    settings.refresh_settings()
    assert editor(settings, "pin").currentData() == ""
    assert editor(settings, "edit").currentData() == "ctrl+win+dragx2"
    settings._reset_mouse_page()
    assert editor(settings, "pin").currentData() == "win+dragleft"
    assert editor(settings, "edit").currentData() == ""
    assert get_quick_capture_bindings(config) == {(frozenset({"ctrl", "win"}), "x2"): "edit"}
    assert settings.apply_settings()
    assert get_quick_capture_bindings(config) == {win(): "pin"}


@pytest.mark.parametrize("stored", ["not a gesture", "win+left", "ctrl+alt+shift+dragleft", "win+dragx3"])
def test_unreadable_saved_value_shows_an_empty_row(qapp, config, stored):
    config.set_app_setting("quick_capture_copy", stored)
    dialog = SimpleNamespace(config_manager=config, tr=lambda text: text)
    card = _create_global_section(dialog, None)
    assert dialog._behavior_controls["quick_capture_copy"].currentData() == ""
    assert (frozenset({"win"}), "left") in get_quick_capture_bindings(config)
    card.deleteLater()


@pytest.mark.parametrize("language", ["en", "ja", "ko", "zh"])
def test_compiled_global_mouse_translations(qapp, language):
    translator = QTranslator()
    path = Path(__file__).parents[1] / "translations" / f"app_{language}.qm"
    assert translator.load(str(path))
    sources = [label for _action, label, *_ in QUICK_CAPTURE_ACTIONS] + [
        "Global Mouse Shortcuts", "Hold modifier keys and drag the mouse", "Back Button Drag",
        "Forward Button Drag", "Capture failed. Please try again.",
    ]
    for source in sources:
        assert translator.translate("SettingsDialog", source), source
