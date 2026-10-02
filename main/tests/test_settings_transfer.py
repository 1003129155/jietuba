# -*- coding: utf-8 -*-
"""设置界面选项导出成 JSON、再导入：导入只填进界面，点应用才按原流程保存；本机相关的项不跟着走。"""
import json
from types import SimpleNamespace

import pytest
from PySide6.QtCore import QSettings
from PySide6.QtGui import QColor

from settings.settings_transfer import (
    FILE_TAG,
    SettingsFileError,
    export_settings,
    read_settings_file,
    recording_reads,
)
from settings.tool_settings import ToolSettingsManager


def _manager(path):
    return ToolSettingsManager(qsettings=QSettings(str(path), QSettings.Format.IniFormat))


def test_export_writes_saved_values_of_the_given_keys_except_local_ones(tmp_path):
    source = _manager(tmp_path / "source.ini")
    source.set_app_setting("hotkey", "ctrl+alt+q")
    source.qsettings.setValue("translation/providers/deepseek/api_key", "sk-source")
    source.set_screenshot_save_path(str(tmp_path / "captures"))
    source.mark_as_run()
    file = tmp_path / "out.json"

    count = export_settings(source, {
        "app/hotkey", "translation/providers/deepseek/api_key",
        "app/screenshot_save_path", "app/has_run_before", "app/smart_selection",
    }, str(file), "9.9.9")

    exported = json.loads(file.read_text(encoding="utf-8"))
    assert exported["format"] == FILE_TAG
    assert exported["app_version"] == "9.9.9"
    # 没保存过的 smart_selection 就是默认值，不写
    assert exported["settings"] == {
        "app/hotkey": "ctrl+alt+q",
        "translation/providers/deepseek/api_key": "sk-source",
    }
    assert count == 2


def test_integers_are_written_as_numbers(tmp_path):
    class Registry:
        values = {"app/ui_scale_percent": 125}

        def contains(self, key):
            return key in self.values

        def value(self, key):
            return self.values[key]

        def sync(self):
            pass

    file = tmp_path / "out.json"
    export_settings(SimpleNamespace(qsettings=Registry()), {"app/ui_scale_percent"}, str(file))

    assert json.loads(file.read_text(encoding="utf-8"))["settings"] == {"app/ui_scale_percent": 125}


@pytest.mark.parametrize("content", [
    "not json",
    json.dumps({"format": "something-else", "version": 1, "settings": {}}),
    json.dumps({"format": FILE_TAG, "version": 1, "settings": ["app/hotkey"]}),
    json.dumps({"format": FILE_TAG, "version": 99, "settings": {}}),
    json.dumps([FILE_TAG]),
])
def test_unusable_files_are_rejected(tmp_path, content):
    file = tmp_path / "bad.json"
    file.write_text(content, encoding="utf-8")
    with pytest.raises(SettingsFileError):
        read_settings_file(str(file))


def test_local_keys_and_odd_values_in_a_file_are_ignored(tmp_path):
    file = tmp_path / "edited.json"
    file.write_text(json.dumps({"format": FILE_TAG, "version": 1, "settings": {
        "app/hotkey": "f2",
        "app/screenshot_save_path": "C:/Users/someone/Pictures",
        "app/has_run_before": False,
        "app/nested": {"a": 1},
    }}), encoding="utf-8")

    assert read_settings_file(str(file)) == {"app/hotkey": "f2"}


def test_recording_reads_collects_keys_and_restores_the_store(tmp_path):
    config = _manager(tmp_path / "s.ini")
    store = config.qsettings

    with recording_reads(config) as keys:
        config.get_hotkey()
        config.get_clipboard_theme()

    assert {"app/hotkey", "clipboard/theme"} <= keys
    assert config.qsettings is store


# ---------------------------------------------------------------- 设置窗口

def _dialog(config, monkeypatch, *, keep=()):
    from ui.settings_ui.dialog import SettingsDialog

    monkeypatch.setattr("ui.settings_ui.dialog.validate_global_hotkey_edits", lambda *_a, **_kw: True)
    dialog = SettingsDialog(config)
    dialog.build_all_pages()
    # 保存这个隔离的窗口不能改到本机的自启、日志和进程里的界面语言
    for attr in ("log_toggle", "autostart_toggle", "language_combo"):
        if attr not in keep:
            delattr(dialog, attr)
    # 有些页面建页时读的是全局配置，按打开窗口时的样子从这份配置回填一遍
    dialog.refresh_settings()
    dialog._settings_snapshot = dialog._snapshot_settings()
    return dialog


def _close(dialog):
    dialog._skip_unsaved_close_prompt = True
    dialog.close()
    dialog.deleteLater()


def _source(tmp_path):
    """外观各项取当前进程正在生效的值：外观管理器是全局单例，应用导入时不能把它们改掉。"""
    from ui.settings_ui.dialog import SettingsDialog

    source = _manager(tmp_path / "source.ini")
    runtime = SettingsDialog._runtime_appearance
    for key in ("ui_theme_mode", "ui_scale_percent", "dialog_scale_percent",
                "selection_border_width", "selection_handle_style", "selection_handle_size"):
        source.set_app_setting(key, runtime(key))
    source.set_app_setting("theme_color", runtime("theme_color").name())
    mask = runtime("mask_color")
    for channel, value in zip("rgb", (mask.red(), mask.green(), mask.blue())):
        source.set_app_setting(f"mask_color_{channel}", value)
    return source


def _file_values(source, tmp_path):
    """source 里存过的全部键，当成一份导出文件。"""
    file = tmp_path / "in.json"
    export_settings(source, source.qsettings.allKeys(), str(file))
    return read_settings_file(str(file))


class _FakeClipboardTheme:
    def __init__(self, name):
        self.current = SimpleNamespace(name=name)
        self.calls = []

    def get_current_theme(self):
        return self.current

    def set_theme(self, name):
        self.calls.append(("theme", name))
        self.current = SimpleNamespace(name=name)
        return True

    def notify_font_size_changed(self, size):
        self.calls.append(("font", size))

    def notify_opacity_changed(self, percent):
        self.calls.append(("opacity", percent))


@pytest.fixture
def config(tmp_path):
    config = _manager(tmp_path / "settings.ini")
    config.set_log_dir(str(tmp_path))
    config.set_screenshot_save_path(str(tmp_path / "captures"))
    config.set_app_setting("hotkey", "ctrl+shift+a")
    return config


@pytest.fixture
def clip_theme(config, monkeypatch):
    """剪贴板主题管理器是全局单例，换成假的，免得改到进程里的剪贴板外观。"""
    fake = _FakeClipboardTheme(config.get_clipboard_theme())
    monkeypatch.setattr("clipboard.ui.theme.themes.get_theme_manager", lambda: fake)
    return fake


@pytest.fixture
def settings(qapp, config, clip_theme, monkeypatch):
    dialog = _dialog(config, monkeypatch)
    yield dialog
    _close(dialog)


def _other_item(combo):
    return next(combo.itemData(i) for i in range(combo.count()) if i != combo.currentIndex())


def test_settings_keys_are_the_options_on_the_settings_pages(settings, config):
    store = config.qsettings

    keys = settings.settings_keys()

    assert {
        "app/hotkey", "app/capture_include_cursor", "pin/hover_buttons",
        "translation/providers/deepseek/api_key", "clipboard/theme", "clipboard/font_size",
        "app/ui_scale_percent", "app/theme_color", "screenshot/scroll_cooldown",
    } <= keys
    assert not any(key.startswith("tools/") for key in keys)
    assert config.qsettings is store


def test_import_fills_the_page_without_saving(settings, config, tmp_path):
    source = _source(tmp_path)
    source.set_app_setting("hotkey", "ctrl+alt+q")
    source.set_app_setting("capture_include_cursor", True)

    settings.load_imported_settings(_file_values(source, tmp_path))

    assert settings.hotkey_input.text() == "ctrl+alt+q"
    assert settings._behavior_controls["capture_include_cursor"].isChecked() is True
    assert settings.save_path_lbl.text() == str(tmp_path / "captures")
    assert settings._has_unsaved_changes()
    assert settings._footer_ok_btn.isEnabled()
    assert config.get_app_setting("hotkey") == "ctrl+shift+a"
    assert config.get_app_setting("capture_include_cursor") is False


def test_apply_after_import_saves_page_options_only(settings, config, tmp_path):
    source = _source(tmp_path)
    source.set_app_setting("hotkey", "ctrl+alt+q")
    source.set_app_setting("pin_hover_buttons", False)
    source.qsettings.setValue("translation/providers/deepseek/api_key", "sk-source")
    source.set_setting("pen", "color", "#123456")
    pen_color = config.get_setting("pen", "color")

    settings.load_imported_settings(_file_values(source, tmp_path))
    assert settings.apply_settings()

    assert config.get_app_setting("hotkey") == "ctrl+alt+q"
    assert config.get_app_setting("pin_hover_buttons") is False
    assert config.qsettings.value("translation/providers/deepseek/api_key") == "sk-source"
    assert config.get_setting("pen", "color") == pen_color
    assert config.get_screenshot_save_path() == str(tmp_path / "captures")
    assert not settings._has_unsaved_changes()


def test_exported_page_options_survive_a_roundtrip(qapp, config, clip_theme, tmp_path, monkeypatch):
    source = _source(tmp_path)
    source.set_app_setting("hotkey", "ctrl+alt+q")
    source.set_app_setting("capture_include_cursor", True)
    source.set_scroll_cooldown(0.4)
    source.qsettings.setValue("translation/providers/deepseek/api_key", "sk-source")
    exporter = _dialog(source, monkeypatch)
    importer = _dialog(config, monkeypatch)
    try:
        file = tmp_path / "roundtrip.json"
        export_settings(source, exporter.settings_keys(), str(file))

        importer.load_imported_settings(read_settings_file(str(file)))
        assert importer.apply_settings()

        assert config.get_app_setting("hotkey") == "ctrl+alt+q"
        assert config.get_app_setting("capture_include_cursor") is True
        assert config.get_scroll_cooldown() == pytest.approx(0.4)
        assert config.qsettings.value("translation/providers/deepseek/api_key") == "sk-source"
    finally:
        _close(exporter)
        _close(importer)


def test_edits_made_after_import_win_over_the_file(settings, config, tmp_path):
    source = _source(tmp_path)
    source.set_app_setting("hotkey", "ctrl+alt+q")
    source.set_app_setting("capture_include_cursor", True)

    settings.load_imported_settings(_file_values(source, tmp_path))
    settings.hotkey_input.setText("ctrl+alt+w")
    assert settings.apply_settings()

    assert config.get_app_setting("hotkey") == "ctrl+alt+w"
    assert config.get_app_setting("capture_include_cursor") is True


def test_reopening_without_saving_drops_the_import(settings, tmp_path):
    source = _source(tmp_path)
    source.set_app_setting("hotkey", "ctrl+alt+q")

    settings.load_imported_settings(_file_values(source, tmp_path))
    settings.refresh_settings()

    assert settings.hotkey_input.text() == "ctrl+shift+a"
    assert not settings._import_unsaved


def test_imported_appearance_is_shown_before_it_takes_effect(settings, tmp_path):
    from core.theme import get_theme
    from core.ui_scale import get_ui_scale

    before = (get_ui_scale().percent, get_theme().theme_color_hex)
    source = _source(tmp_path)
    other_percent = _other_item(settings._ui_scale_combo)
    source.set_app_setting("ui_scale_percent", other_percent)
    source.set_app_setting("theme_color", "#123456")

    settings.load_imported_settings(_file_values(source, tmp_path))

    assert settings._ui_scale_combo.currentData() == other_percent
    assert settings._appearance_theme_color == QColor("#123456")
    assert (get_ui_scale().percent, get_theme().theme_color_hex) == before


def test_clipboard_appearance_is_saved_on_apply_like_other_settings(settings, config, clip_theme):
    font = _other_item(settings._clip_font_combo)
    opacity = _other_item(settings._clip_opacity_combo)
    theme = next(name for name in ("dark", "light") if name != config.get_clipboard_theme())

    settings._clip_font_combo.setCurrentIndex(settings._clip_font_combo.findData(font))
    settings._clip_opacity_combo.setCurrentIndex(settings._clip_opacity_combo.findData(opacity))
    settings._clip_theme_name = theme

    assert settings._has_unsaved_changes()
    assert clip_theme.calls == []
    assert config.get_clipboard_font_size() != font

    assert settings.apply_settings()

    assert (config.get_clipboard_theme(), config.get_clipboard_font_size(),
            config.get_clipboard_window_opacity()) == (theme, font, opacity)
    assert clip_theme.calls == [("theme", theme), ("font", font), ("opacity", opacity)]


def test_unchanged_clipboard_appearance_is_not_reapplied(settings, clip_theme):
    assert settings.apply_settings()
    assert clip_theme.calls == []


def test_imported_clipboard_appearance_waits_for_apply(settings, config, clip_theme, tmp_path):
    source = _source(tmp_path)
    theme = next(name for name in ("dark", "light") if name != config.get_clipboard_theme())
    font = _other_item(settings._clip_font_combo)
    source.set_clipboard_theme(theme)
    source.set_clipboard_font_size(font)

    settings.load_imported_settings(_file_values(source, tmp_path))

    assert settings._clip_theme_name == theme
    assert settings._clip_font_combo.currentData() == font
    assert clip_theme.calls == []

    assert settings.apply_settings()

    assert (config.get_clipboard_theme(), config.get_clipboard_font_size()) == (theme, font)
    assert clip_theme.calls == [("theme", theme), ("font", font)]


def test_language_change_from_an_import_is_detected(qapp, config, clip_theme, tmp_path, monkeypatch):
    from core.i18n import I18nManager

    loaded = []
    monkeypatch.setattr(I18nManager, "load_language", staticmethod(loaded.append))
    config.set_app_setting("language", "zh")
    dialog = _dialog(config, monkeypatch, keep=("language_combo",))
    try:
        source = _source(tmp_path)
        source.set_app_setting("language", "en")

        dialog.load_imported_settings(_file_values(source, tmp_path))
        assert dialog.apply_settings()

        assert loaded == ["en"]
        assert dialog._language_changed_on_apply
        assert config.get_app_setting("language") == "en"
    finally:
        _close(dialog)


# ---------------------------------------------------------------- 按钮

@pytest.fixture
def dialogs(monkeypatch):
    shown = []
    for name in ("show_info_dialog", "show_error_dialog"):
        monkeypatch.setattr(f"ui.settings_ui.page_misc.{name}",
                            lambda _parent, _title, message, name=name: shown.append((name, message)))
    return shown


def _choose_file(monkeypatch, path, method):
    monkeypatch.setattr(f"ui.settings_ui.page_misc.{method}",
                        lambda *_a, **_kw: (str(path), ""))


def test_export_button_applies_pending_edits_and_writes_page_options(settings, config, tmp_path,
                                                                    monkeypatch, dialogs):
    config.set_setting("pen", "color", "#123456")
    settings._behavior_controls["capture_include_cursor"].setChecked(True)
    file = tmp_path / "out.json"
    _choose_file(monkeypatch, file, "get_save_file_name")

    settings.export_settings_button.click()

    exported = json.loads(file.read_text(encoding="utf-8"))["settings"]
    assert exported["app/capture_include_cursor"] in (True, "true")
    assert exported["app/hotkey"] == "ctrl+shift+a"
    assert not any(key.startswith("tools/") for key in exported)
    assert "app/screenshot_save_path" not in exported
    assert not settings._has_unsaved_changes()
    assert dialogs == [("show_info_dialog", f"Settings exported to {file}")]


def test_import_button_loads_the_file_into_the_page(settings, config, tmp_path, monkeypatch, dialogs):
    source = _source(tmp_path)
    source.set_app_setting("hotkey", "ctrl+alt+q")
    file = tmp_path / "picked.json"
    export_settings(source, source.qsettings.allKeys(), str(file))
    _choose_file(monkeypatch, file, "get_open_file_name")

    settings.import_settings_button.click()

    assert settings.hotkey_input.text() == "ctrl+alt+q"
    assert config.get_app_setting("hotkey") == "ctrl+shift+a"
    assert dialogs == [("show_info_dialog", "Settings loaded. Click Apply to use them.")]


def test_importing_a_bad_file_reports_it_and_changes_nothing(settings, tmp_path, monkeypatch, dialogs):
    file = tmp_path / "in.json"
    file.write_text("{}", encoding="utf-8")
    _choose_file(monkeypatch, file, "get_open_file_name")

    settings.import_settings_button.click()

    assert not settings._import_unsaved
    assert settings.hotkey_input.text() == "ctrl+shift+a"
    assert [name for name, _ in dialogs] == ["show_error_dialog"]
