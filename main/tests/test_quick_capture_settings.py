"""Quick capture preferences persist and participate in normal settings workflows."""

from pathlib import Path
from types import SimpleNamespace

import pytest
from PySide6.QtCore import QSettings, QTranslator

from settings.tool_settings import (
    ToolSettingsManager, dump_quick_capture_bindings, parse_quick_capture_bindings,
)
from ui.settings_ui.dialog import SettingsDialog
from ui.settings_ui.page_hotkey import _create_quick_capture


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
    # Saving this isolated dialog must not change machine autostart or logging.
    for attr in ("log_toggle", "autostart_toggle", "language_combo"):
        delattr(dialog, attr)
    yield dialog
    dialog._skip_unsaved_close_prompt = True
    dialog.close()
    dialog.deleteLater()


def editor(dialog):
    return dialog._behavior_controls["quick_capture_bindings"]


def saved(config):
    return parse_quick_capture_bindings(config.get_app_setting("quick_capture_bindings"))


def choose(combo, value):
    index = combo.findData(value)
    assert index >= 0
    combo.setCurrentIndex(index)


WIN = frozenset({"win"})
SHIFT_WIN = frozenset({"shift", "win"})
CTRL_WIN = frozenset({"ctrl", "win"})


def test_off_by_default_with_no_gestures(settings, config):
    assert editor(settings).bindings() == ()
    assert editor(settings)._empty.isVisibleTo(settings)
    assert saved(config) == ()


def test_added_gestures_suggest_unused_combinations_and_actions(settings):
    for _ in range(3):
        editor(settings).add_suggested()
    assert editor(settings).bindings() == (
        (WIN, "copy_pin"), (SHIFT_WIN, "pin"), (CTRL_WIN, "copy"),
    )
    assert not editor(settings)._empty.isVisibleTo(settings)


def test_add_stops_once_every_combination_is_used(settings):
    for _ in range(20):
        editor(settings).add_suggested()
    combinations = [modifiers for modifiers, _action in editor(settings).bindings()]
    assert len(combinations) == 10 == len(set(combinations))
    assert not editor(settings)._add.isEnabled()


def test_apply_persists_gestures_and_they_reload(settings, config):
    editor(settings).add_suggested()
    editor(settings).add_suggested()
    row = editor(settings)._rows[1]
    choose(row._first, "ctrl")
    choose(row._second, "alt")
    choose(row._action, "edit")
    assert settings._has_unsaved_changes()
    assert settings.apply_settings()
    assert not settings._has_unsaved_changes()
    expected = ((WIN, "copy_pin"), (frozenset({"ctrl", "alt"}), "edit"))
    assert saved(config) == expected
    restored = ToolSettingsManager(qsettings=QSettings(config.qsettings.fileName(), QSettings.IniFormat))
    assert saved(restored) == expected


def test_removing_a_gesture(settings, config):
    editor(settings).add_suggested()
    editor(settings).add_suggested()
    editor(settings)._rows[0].remove_requested.emit(editor(settings)._rows[0])
    assert editor(settings).bindings() == ((SHIFT_WIN, "pin"),)
    assert settings.apply_settings()
    assert saved(config) == ((SHIFT_WIN, "pin"),)


def test_every_gesture_needs_a_modifier_and_a_repeated_key_counts_once(settings):
    editor(settings).add_suggested()
    row = editor(settings)._rows[0]
    assert "" not in [row._first.itemData(i) for i in range(row._first.count())]
    choose(row._second, "win")
    assert row.modifiers() == WIN
    assert row._second.currentData() == ""


def test_same_combination_twice_blocks_apply(settings, config, monkeypatch):
    warnings = []
    monkeypatch.setattr("ui.settings_ui.dialog.show_warning_dialog", lambda *args: warnings.append(args))
    editor(settings).add_suggested()
    editor(settings).add_suggested()
    choose(editor(settings)._rows[1]._first, "win")
    choose(editor(settings)._rows[1]._second, "")
    assert not settings.apply_settings()
    assert "Win + Left Drag: Gesture 1 / Gesture 2" in warnings[0][2]
    assert saved(config) == ()


def test_refresh_uses_saved_value_and_reset_is_not_saved_implicitly(settings, config):
    config.set_app_setting("quick_capture_bindings", dump_quick_capture_bindings(((CTRL_WIN, "edit"),)))
    settings.refresh_settings()
    assert editor(settings).bindings() == ((CTRL_WIN, "edit"),)
    settings._reset_hotkey_page()
    assert editor(settings).bindings() == ()
    assert saved(config) == ((CTRL_WIN, "edit"),)
    assert settings.apply_settings()
    assert saved(config) == ()


def test_unreadable_saved_value_shows_no_gestures(qapp, config):
    config.set_app_setting("quick_capture_bindings", "not json")
    dialog = SimpleNamespace(config_manager=config, tr=lambda text: text)
    card = _create_quick_capture(dialog, None)
    assert dialog._behavior_controls["quick_capture_bindings"].bindings() == ()
    card.deleteLater()


@pytest.mark.parametrize("language", ["en", "ja", "ko", "zh"])
def test_compiled_quick_capture_translations(qapp, language):
    translator = QTranslator()
    path = Path(__file__).parents[1] / "translations" / f"app_{language}.qm"
    assert translator.load(str(path))
    for source in (
        "Quick Capture", "First Modifier", "Second Modifier", "Capture and Pin", "Capture and Copy",
        "Quick capture failed. Please try again.",
        "Capture, Copy and Pin", "Normal Capture",
        "Add Gesture", "Remove Gesture", "Gesture %1", "No gestures yet. Quick Capture is off.",
        "Hold the modifier keys and drag with the left mouse button. Release to capture, Esc to cancel. "
        "Some security software may warn about keyboard monitoring once a gesture is added; "
        "allow it to keep using Quick Capture.",
    ):
        assert translator.translate("SettingsDialog", source)
