# -*- coding: utf-8 -*-
"""
设置页纯逻辑测试（ui/settings_ui 下的三个大页面 + 翻译页）

page_clipboard.py(435 语句/8.0%)、page_hotkey.py(218/8.3%)、
page_appearance.py(182/7.7%) 都不是类，而是 create_*_page(dialog) 工厂函数，
构造期就会实例化几十个 Fluent 控件并拉起主题、剪贴板、翻译等子系统，
整页构造的隔离成本远高于收益。

但这些文件里真正会出错的逻辑是可以脱离界面运行的：应用内快捷键的同组冲突
检测与撤销回退、存储体积的单位换算、翻译服务商切换时哪几组控件该显示。
这几处一旦改错，用户看到的是"快捷键设了没生效""切换服务商后填错框"，
而现有测试一条都不覆盖。

隔离方式：直接调用模块级函数，用假 dialog / 假控件充当参数。
弹窗函数 show_confirm_dialog 是各 page 文件顶部 import 的，
因此 patch 目标是 page 文件自己的命名空间，而不是 ui.dialogs。
"""
from types import SimpleNamespace

from ui.settings_ui import page_appearance, page_clipboard, page_hotkey, page_translation


class _Edit:
    """假快捷键输入框：记录 setText 与 blockSignals 的调用"""

    def __init__(self, text=""):
        self._text = text
        self.set_texts = []
        self.block_calls = []

    def text(self):
        return self._text

    def setText(self, value):
        self._text = value
        self.set_texts.append(value)

    def blockSignals(self, value):
        self.block_calls.append(value)


class _Visibility:
    def __init__(self):
        self.calls = []

    def setVisible(self, value):
        self.calls.append(value)

    @property
    def visible(self):
        return self.calls[-1] if self.calls else None


# ============================================================================
# page_clipboard：体积换算与目标目录判定
# ============================================================================

class TestFormatBytes:

    def test_bytes_below_one_kilobyte_keep_the_raw_count(self):
        for value in (0, 1, 512, 1023):
            assert page_clipboard._format_bytes(value) == f"{value} B"

    def test_kilobyte_range_uses_one_decimal(self):
        assert page_clipboard._format_bytes(1024) == "1.0 KB"
        assert page_clipboard._format_bytes(1536) == "1.5 KB"
        assert page_clipboard._format_bytes(1024 ** 2 - 1) == "1024.0 KB"

    def test_megabyte_range_uses_one_decimal(self):
        assert page_clipboard._format_bytes(1024 ** 2) == "1.0 MB"
        assert page_clipboard._format_bytes(int(2.5 * 1024 ** 2)) == "2.5 MB"

    def test_gigabyte_range_uses_two_decimals(self):
        assert page_clipboard._format_bytes(1024 ** 3) == "1.00 GB"
        assert page_clipboard._format_bytes(int(1.25 * 1024 ** 3)) == "1.25 GB"

    def test_unit_boundaries_switch_exactly_at_the_power_of_1024(self):
        assert page_clipboard._format_bytes(1023).endswith(" B")
        assert page_clipboard._format_bytes(1024).endswith(" KB")
        assert page_clipboard._format_bytes(1024 ** 2 - 1).endswith(" KB")
        assert page_clipboard._format_bytes(1024 ** 2).endswith(" MB")
        assert page_clipboard._format_bytes(1024 ** 3 - 1).endswith(" MB")
        assert page_clipboard._format_bytes(1024 ** 3).endswith(" GB")


class TestTargetHasClipboardData:

    def test_empty_directory_has_no_data(self, tmp_path):
        assert page_clipboard._target_has_clipboard_data(str(tmp_path)) is False

    def test_any_database_sidecar_counts_as_existing_data(self, tmp_path):
        """-wal / -shm 是 SQLite 的伴生文件，只有它们也说明数据在那里"""
        for suffix in ("", "-wal", "-shm"):
            target = tmp_path / f"case{suffix or 'main'}"
            target.mkdir()
            (target / f"clipboard.db{suffix}").write_bytes(b"x")
            assert page_clipboard._target_has_clipboard_data(str(target)) is True, suffix

    def test_empty_images_directory_does_not_count(self, tmp_path):
        (tmp_path / "images").mkdir()
        assert page_clipboard._target_has_clipboard_data(str(tmp_path)) is False

    def test_non_empty_images_directory_counts(self, tmp_path):
        images = tmp_path / "images"
        images.mkdir()
        (images / "a.png").write_bytes(b"x")
        assert page_clipboard._target_has_clipboard_data(str(tmp_path)) is True

    def test_unrelated_files_do_not_count(self, tmp_path):
        (tmp_path / "notes.txt").write_bytes(b"x")
        assert page_clipboard._target_has_clipboard_data(str(tmp_path)) is False


class TestApplySizeToLabel:

    def test_size_is_written_into_the_label(self):
        label = _Edit()
        page_clipboard._apply_size_to_label(label, "3.0 MB")
        assert label.set_texts == ["3.0 MB"]

    def test_empty_size_becomes_a_dash(self):
        label = _Edit()
        page_clipboard._apply_size_to_label(label, "")
        assert label.set_texts == ["—"]

    def test_dead_state_skips_the_update(self):
        """异步算完时页面可能已经关掉，state['alive'] 是那道闸门"""
        label = _Edit()
        page_clipboard._apply_size_to_label(label, "3.0 MB", {"alive": False})
        assert label.set_texts == []

    def test_alive_state_allows_the_update(self):
        label = _Edit()
        page_clipboard._apply_size_to_label(label, "3.0 MB", {"alive": True})
        assert label.set_texts == ["3.0 MB"]

    def test_state_without_the_alive_key_defaults_to_alive(self):
        label = _Edit()
        page_clipboard._apply_size_to_label(label, "3.0 MB", {})
        assert label.set_texts == ["3.0 MB"]

    def test_missing_label_is_tolerated(self):
        page_clipboard._apply_size_to_label(None, "3.0 MB")

    def test_destroyed_widget_does_not_propagate_its_runtime_error(self):
        class _Destroyed:
            def setText(self, value):
                raise RuntimeError("wrapped C/C++ object has been deleted")

        page_clipboard._apply_size_to_label(_Destroyed(), "3.0 MB")


# ============================================================================
# page_hotkey：布局高度与同组冲突检测
# ============================================================================

class TestStackPageHeight:

    def test_no_rows_take_no_height(self):
        assert page_hotkey._stack_page_height(0) == 0

    def test_single_row_has_no_spacing(self):
        assert page_hotkey._stack_page_height(1) == 46

    def test_each_extra_row_adds_its_height_plus_one_gap(self):
        for rows, expected in ((2, 100), (3, 154), (8, 424)):
            assert page_hotkey._stack_page_height(rows) == expected, rows


class TestShortcutKeyTables:
    """常量表是冲突检测和默认值回退的数据源，结构错了两处逻辑一起失效"""

    def test_every_entry_is_a_key_label_default_triple(self):
        for table in (page_hotkey.SCREENSHOT_KEYS, page_hotkey.PIN_KEYS):
            for entry in table:
                assert len(entry) == 3, entry
                cfg_key, label, default = entry
                assert cfg_key.startswith("inapp_"), cfg_key
                assert label and isinstance(label, str)
                assert default and isinstance(default, str)

    def test_combined_table_is_the_concatenation_of_both_groups(self):
        assert page_hotkey.INAPP_KEYS == page_hotkey.SCREENSHOT_KEYS + page_hotkey.PIN_KEYS

    def test_config_keys_are_unique_within_each_group(self):
        for table in (page_hotkey.SCREENSHOT_KEYS, page_hotkey.PIN_KEYS):
            keys = [entry[0] for entry in table]
            assert len(keys) == len(set(keys)), keys


def _hotkey_dialog(edits, groups, stored=None):
    stored = stored or {}
    return SimpleNamespace(
        _inapp_edits=edits,
        _inapp_groups=groups,
        config_manager=SimpleNamespace(get_inapp_shortcut=lambda key: stored.get(key, "")),
        tr=lambda text: text,
    )


class TestShortcutConflictDetection:

    def test_blank_input_is_ignored(self, monkeypatch):
        asked = []
        monkeypatch.setattr(page_hotkey, "show_confirm_dialog",
                            lambda *a, **k: asked.append(a) or True)
        edits = {"inapp_undo": _Edit("ctrl+z"), "inapp_redo": _Edit("ctrl+z")}
        dialog = _hotkey_dialog(edits, {"inapp_undo": "shot", "inapp_redo": "shot"})
        for text in ("", "   "):
            page_hotkey._on_shortcut_changed(dialog, "inapp_redo", text, "")
        assert asked == []

    def test_half_typed_combination_is_ignored(self, monkeypatch):
        asked = []
        monkeypatch.setattr(page_hotkey, "show_confirm_dialog",
                            lambda *a, **k: asked.append(a) or True)
        edits = {"inapp_undo": _Edit("ctrl+"), "inapp_redo": _Edit("ctrl+")}
        dialog = _hotkey_dialog(edits, {"inapp_undo": "shot", "inapp_redo": "shot"})
        page_hotkey._on_shortcut_changed(dialog, "inapp_redo", "ctrl+", "")
        assert asked == []

    def test_distinct_shortcuts_raise_no_conflict(self, monkeypatch):
        asked = []
        monkeypatch.setattr(page_hotkey, "show_confirm_dialog",
                            lambda *a, **k: asked.append(a) or True)
        edits = {"inapp_undo": _Edit("ctrl+z"), "inapp_redo": _Edit("ctrl+y")}
        dialog = _hotkey_dialog(edits, {"inapp_undo": "shot", "inapp_redo": "shot"})
        page_hotkey._on_shortcut_changed(dialog, "inapp_redo", "ctrl+y", "")
        assert asked == []

    def test_same_shortcut_in_a_different_group_is_allowed(self, monkeypatch):
        """
        截图组和贴图组是两套独立的按键上下文，两边都用 ctrl+c 并不冲突——
        这正是 _inapp_groups 存在的原因。
        """
        asked = []
        monkeypatch.setattr(page_hotkey, "show_confirm_dialog",
                            lambda *a, **k: asked.append(a) or True)
        edits = {"inapp_confirm": _Edit("ctrl+c"), "inapp_copy_pin": _Edit("ctrl+c")}
        dialog = _hotkey_dialog(
            edits, {"inapp_confirm": "shot", "inapp_copy_pin": "pin"})
        page_hotkey._on_shortcut_changed(dialog, "inapp_copy_pin", "ctrl+c", "")
        assert asked == []

    def test_comparison_ignores_case_and_padding(self, monkeypatch):
        asked = []
        monkeypatch.setattr(page_hotkey, "show_confirm_dialog",
                            lambda *a, **k: asked.append(a) or True)
        edits = {"inapp_undo": _Edit("  CTRL+Z  "), "inapp_redo": _Edit("x")}
        dialog = _hotkey_dialog(edits, {"inapp_undo": "shot", "inapp_redo": "shot"})
        page_hotkey._on_shortcut_changed(dialog, "inapp_redo", " Ctrl+Z ", "")
        assert len(asked) == 1

    def test_accepting_the_prompt_clears_the_older_binding(self, monkeypatch):
        monkeypatch.setattr(page_hotkey, "show_confirm_dialog", lambda *a, **k: True)
        edits = {"inapp_undo": _Edit("ctrl+z"), "inapp_redo": _Edit("ctrl+z")}
        dialog = _hotkey_dialog(edits, {"inapp_undo": "shot", "inapp_redo": "shot"})
        page_hotkey._on_shortcut_changed(dialog, "inapp_redo", "ctrl+z", "")
        assert edits["inapp_undo"].set_texts == [""]
        assert edits["inapp_redo"].set_texts == []

    def test_declining_the_prompt_restores_the_stored_value(self, monkeypatch):
        monkeypatch.setattr(page_hotkey, "show_confirm_dialog", lambda *a, **k: False)
        edits = {"inapp_undo": _Edit("ctrl+z"), "inapp_redo": _Edit("ctrl+z")}
        dialog = _hotkey_dialog(
            edits, {"inapp_undo": "shot", "inapp_redo": "shot"},
            stored={"inapp_redo": "ctrl+shift+y"})
        page_hotkey._on_shortcut_changed(dialog, "inapp_redo", "ctrl+z", "")
        assert edits["inapp_redo"].set_texts == ["ctrl+shift+y"]
        assert edits["inapp_undo"].set_texts == []

    def test_declining_without_a_stored_value_restores_the_factory_default(self, monkeypatch):
        monkeypatch.setattr(page_hotkey, "show_confirm_dialog", lambda *a, **k: False)
        edits = {"inapp_undo": _Edit("ctrl+z"), "inapp_redo": _Edit("ctrl+z")}
        dialog = _hotkey_dialog(edits, {"inapp_undo": "shot", "inapp_redo": "shot"})
        page_hotkey._on_shortcut_changed(dialog, "inapp_redo", "ctrl+z", "")
        # 表里 inapp_redo 的默认值
        assert edits["inapp_redo"].set_texts == ["ctrl+y"]

    def test_signals_are_blocked_and_released_around_the_prompt(self, monkeypatch):
        """回填文本会再次触发 textChanged，必须先屏蔽信号否则递归"""
        monkeypatch.setattr(page_hotkey, "show_confirm_dialog", lambda *a, **k: True)
        edits = {"inapp_undo": _Edit("ctrl+z"), "inapp_redo": _Edit("ctrl+z")}
        dialog = _hotkey_dialog(edits, {"inapp_undo": "shot", "inapp_redo": "shot"})
        page_hotkey._on_shortcut_changed(dialog, "inapp_redo", "ctrl+z", "")
        for edit in edits.values():
            assert edit.block_calls == [True, False]

    def test_prompt_message_names_the_conflicting_action(self, monkeypatch):
        captured = {}

        def _fake_confirm(parent, title, message):
            captured["title"] = title
            captured["message"] = message
            return True

        monkeypatch.setattr(page_hotkey, "show_confirm_dialog", _fake_confirm)
        edits = {"inapp_undo": _Edit("ctrl+z"), "inapp_redo": _Edit("ctrl+z")}
        dialog = _hotkey_dialog(edits, {"inapp_undo": "shot", "inapp_redo": "shot"})
        page_hotkey._on_shortcut_changed(dialog, "inapp_redo", "ctrl+z", "")
        assert captured["title"] == "Shortcut Conflict"
        # 占位符必须被真正替换掉，而不是把 %1/%2 显示给用户
        assert "%1" not in captured["message"]
        assert "%2" not in captured["message"]
        assert "CTRL+Z" in captured["message"]
        assert "Undo" in captured["message"]


# ============================================================================
# page_appearance：主题色常量
# ============================================================================

class TestThemeColourTable:

    def test_every_entry_is_a_name_accent_background_triple(self):
        for entry in page_appearance._THEME_COLORS:
            assert len(entry) == 3, entry
            name, accent, background = entry
            assert name and isinstance(name, str)
            for colour in (accent, background):
                assert colour.startswith("#"), colour
                assert len(colour) == 7, colour
                int(colour[1:], 16)  # 必须是合法十六进制

    def test_theme_names_are_unique(self):
        names = [entry[0] for entry in page_appearance._THEME_COLORS]
        assert len(names) == len(set(names)), names

    def test_light_and_dark_themes_are_both_offered(self):
        names = [entry[0] for entry in page_appearance._THEME_COLORS]
        assert "light" in names
        assert "dark" in names


# ============================================================================
# page_translation：服务商切换的控件显隐
# ============================================================================

PROVIDER_GROUPS = {
    "deepl": "deepl_settings_group",
    "amazon": "amazon_translate_settings_group",
    "google": "google_translate_settings_group",
    "azure": "azure_translate_settings_group",
}


def _translation_dialog(provider, with_optional=False):
    dialog = SimpleNamespace(
        translation_provider_combo=SimpleNamespace(currentData=lambda: provider))
    for attr in PROVIDER_GROUPS.values():
        setattr(dialog, attr, _Visibility())
    if with_optional:
        for attr in ("deepl_translation_info_label", "split_sentences_toggle",
                     "preserve_formatting_toggle"):
            setattr(dialog, attr, _Visibility())
    return dialog


class TestProviderGroupVisibility:

    def test_only_the_selected_provider_group_is_visible(self):
        for provider, visible_attr in PROVIDER_GROUPS.items():
            dialog = _translation_dialog(provider)
            page_translation._update_provider_groups(dialog)
            for attr in PROVIDER_GROUPS.values():
                expected = attr == visible_attr
                assert getattr(dialog, attr).visible is expected, (provider, attr)

    def test_an_unknown_provider_hides_every_group(self):
        """服务商列表由 registry 动态给出，出现未知值时不能留着上一个的输入框"""
        for provider in (None, "", "some_future_provider"):
            dialog = _translation_dialog(provider)
            page_translation._update_provider_groups(dialog)
            for attr in PROVIDER_GROUPS.values():
                assert getattr(dialog, attr).visible is False, (provider, attr)

    def test_deepl_only_options_follow_the_deepl_selection(self):
        for provider in ("deepl", "google", "amazon", "azure"):
            dialog = _translation_dialog(provider, with_optional=True)
            page_translation._update_provider_groups(dialog)
            expected = provider == "deepl"
            assert dialog.deepl_translation_info_label.visible is expected, provider
            assert dialog.split_sentences_toggle.visible is expected, provider
            assert dialog.preserve_formatting_toggle.visible is expected, provider

    def test_optional_widgets_absent_before_the_page_is_built(self):
        dialog = _translation_dialog("deepl", with_optional=False)
        page_translation._update_provider_groups(dialog)
        assert dialog.deepl_settings_group.visible is True
