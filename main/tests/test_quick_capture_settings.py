"""Quick capture preferences persist and participate in normal settings workflows."""

from pathlib import Path
from types import SimpleNamespace

import pytest
from PySide6.QtCore import QSettings, QTranslator

from settings.tool_settings import ToolSettingsManager
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


def choose(dialog, key, value):
    control = dialog._behavior_controls[f"quick_capture_{key}"]
    index = control.findData(value)
    assert index >= 0
    control.setCurrentIndex(index)


def values(dialog):
    return tuple(dialog._behavior_controls[f"quick_capture_{key}"].currentData()
                 for key in ("modifier_1", "modifier_2", "action"))


def test_defaults_and_modifier_only_choices(settings, config):
    assert values(settings) == ("win", "", "copy_pin")
    for key in ("modifier_1", "modifier_2"):
        combo = settings._behavior_controls[f"quick_capture_{key}"]
        assert [combo.itemData(i) for i in range(combo.count())] == ["", "ctrl", "shift", "win", "alt"]
        assert not combo.isEditable()
    assert config.get_app_setting("quick_capture_action") == "copy_pin"


@pytest.mark.parametrize("action", ["none", "pin", "copy", "copy_pin", "edit"])
def test_apply_persists_modifiers_and_action(settings, config, action):
    choose(settings, "modifier_1", "ctrl")
    choose(settings, "modifier_2", "shift")
    choose(settings, "action", action)
    assert settings._has_unsaved_changes()
    assert settings.apply_settings()
    assert not settings._has_unsaved_changes()
    assert config.get_app_setting("quick_capture_modifier_1") == "ctrl"
    assert config.get_app_setting("quick_capture_modifier_2") == "shift"
    assert config.get_app_setting("quick_capture_action") == action
    restored = ToolSettingsManager(qsettings=QSettings(config.qsettings.fileName(), QSettings.IniFormat))
    assert restored.get_app_setting("quick_capture_modifier_1") == "ctrl"
    assert restored.get_app_setting("quick_capture_modifier_2") == "shift"
    assert restored.get_app_setting("quick_capture_action") == action


def test_no_modifiers_can_be_saved_to_disable_quick_capture(settings, config):
    choose(settings, "modifier_1", "")
    choose(settings, "modifier_2", "")
    assert settings.apply_settings()
    assert config.get_app_setting("quick_capture_modifier_1") == ""
    assert config.get_app_setting("quick_capture_modifier_2") == ""


def test_duplicate_modifiers_are_collapsed_from_either_combo(settings):
    choose(settings, "modifier_2", "win")
    assert values(settings)[:2] == ("win", "")
    choose(settings, "modifier_2", "shift")
    choose(settings, "modifier_1", "shift")
    assert values(settings)[:2] == ("shift", "")


def test_refresh_uses_saved_values_and_reset_is_not_saved_implicitly(settings, config):
    config.set_app_setting("quick_capture_modifier_1", "alt")
    config.set_app_setting("quick_capture_modifier_2", "ctrl")
    config.set_app_setting("quick_capture_action", "edit")
    settings.refresh_settings()
    assert values(settings) == ("alt", "ctrl", "edit")
    settings._reset_hotkey_page()
    assert values(settings) == ("win", "", "copy_pin")
    assert config.get_app_setting("quick_capture_action") == "edit"
    assert settings.apply_settings()
    assert config.get_app_setting("quick_capture_action") == "copy_pin"


def test_invalid_saved_values_fall_back_to_defaults(settings, config):
    for key in ("modifier_1", "modifier_2", "action"):
        config.set_app_setting(f"quick_capture_{key}", "unknown")
    settings.refresh_settings()
    assert values(settings) == ("win", "", "copy_pin")


def test_initial_invalid_values_fall_back_to_defaults(qapp, config):
    for key in ("modifier_1", "modifier_2", "action"):
        config.set_app_setting(f"quick_capture_{key}", "unknown")
    dialog = SimpleNamespace(config_manager=config, tr=lambda text: text)
    card = _create_quick_capture(dialog, None)
    assert values(dialog) == ("win", "", "copy_pin")
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
        "Hold the modifier keys and drag with the left mouse button. Release to capture, Esc to cancel. "
        "Select at least one modifier to enable Quick Capture.",
    ):
        assert translator.translate("SettingsDialog", source)
