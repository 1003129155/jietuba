# -*- coding: utf-8 -*-
"""放大镜颜色格式的模型与管理窗口

格式只有固定的预设，用户能改的是勾选和顺序。这里盯住模板渲染的正确性，以及
存档不对劲时的兜底：一个格式都没勾、缺了预设、混进了预设以外的条目。
"""
import json
from types import SimpleNamespace

import pytest
from PySide6.QtGui import QColor

from settings import color_formats
from settings.color_formats import ColorFormat

SAMPLE = QColor(230, 153, 60)


class _Config:
    def __init__(self, **values):
        self.values = dict(values)

    def get_app_setting(self, key, default=None):
        return self.values.get(key, default)

    def set_app_setting(self, key, value):
        self.values[key] = value


class TestRender:

    def test_builtin_templates_render_ready_to_paste_strings(self):
        rendered = {f.name: f.render(SAMPLE) for f in color_formats.default_formats()}
        assert rendered == {
            "RGB + HEX": "230, 153, 60  #E6993C",
            "RGB": "230, 153, 60",
            "CSS rgb()": "rgb(230, 153, 60)",
            "HEX": "#E6993C",
            "HEX without #": "E6993C",
            "CSS hsl()": "hsl(32, 77%, 57%)",
        }

    def test_hex_keeps_two_digits_per_channel(self):
        """QColor.name() 会给出 #0a0b0c，自己拼的话别把前导零丢了。"""
        assert ColorFormat("", "#{hex}").render(QColor(10, 11, 12)) == "#0A0B0C"

    def test_grey_has_no_hue_and_still_renders_valid_css(self):
        """无彩色的 hslHue() 是 -1，原样写进 hsl() 就成了非法的 CSS。"""
        assert ColorFormat("", "hsl({h}, {s}%, {l}%)").render(
            QColor(128, 128, 128)) == "hsl(0, 0%, 50%)"


class TestNormalize:

    def test_missing_presets_are_added_back_unchecked(self):
        result = color_formats.normalize([ColorFormat("RGB", "{r}, {g}, {b}", enabled=True)])
        names = [f.name for f in result]
        assert names[0] == "RGB"
        assert set(names) == {name for name, _ in color_formats.BUILTIN_FORMATS}
        assert [f.name for f in result if f.enabled] == ["RGB"]

    def test_formats_outside_the_presets_are_dropped(self):
        custom = ColorFormat("Unity", "Color({r}, {g}, {b})", enabled=True)
        result = color_formats.normalize([custom])
        assert "Unity" not in [f.name for f in result]
        assert len(result) == len(color_formats.BUILTIN_FORMATS)

    def test_templates_always_come_from_the_presets(self):
        result = color_formats.normalize([ColorFormat("HEX", "{r[0]}", enabled=True)])
        assert result[0].render(SAMPLE) == "#E6993C"

    def test_duplicate_names_keep_only_the_first(self):
        result = color_formats.normalize([
            ColorFormat("HEX", "", enabled=True), ColorFormat("HEX", "", enabled=False)])
        assert [f.name for f in result].count("HEX") == 1
        assert result[0].enabled

    def test_something_stays_enabled_when_everything_was_unchecked(self):
        """一个都没勾的话放大镜就没东西可显示了，回落到第一条。"""
        formats = [ColorFormat(n, t, enabled=False)
                   for n, t in color_formats.BUILTIN_FORMATS]
        assert color_formats.normalize(formats)[0].enabled


class TestLoadSave:

    def test_round_trip_keeps_order_and_checks(self):
        config = _Config()
        edited = [
            ColorFormat("CSS rgb()", "rgb({r}, {g}, {b})", enabled=True),
            ColorFormat("HEX", "#{hex}", enabled=True),
        ]
        color_formats.save(config, edited)
        loaded = color_formats.load(config)
        assert [f.name for f in loaded][:2] == ["CSS rgb()", "HEX"]
        assert [f.name for f in loaded if f.enabled] == ["CSS rgb()", "HEX"]

    def test_stored_custom_entries_and_templates_are_ignored(self):
        """开发期的存档带过自定义格式和模板，读进来只认预设名。"""
        config = _Config(**{color_formats.SETTING_KEY: json.dumps([
            {"name": "Unity", "template": "Color({r})", "enabled": True, "builtin": False},
            {"name": "HEX", "template": "{r.x}", "enabled": True, "builtin": True},
        ])})
        loaded = color_formats.load(config)
        assert [f.name for f in loaded if f.enabled] == ["HEX"]
        assert loaded[0].render(SAMPLE) == "#E6993C"

    def test_legacy_single_choice_is_migrated(self):
        """老版本存的是单选的一个键，升级后要变成「只勾了这一个」的列表。"""
        config = _Config(magnifier_color_copy_format="hex")
        loaded = color_formats.load(config)
        assert [f.name for f in loaded if f.enabled] == ["HEX"]

    def test_legacy_default_keeps_the_original_default_checked(self):
        assert [f.name for f in color_formats.load(_Config()) if f.enabled] == ["RGB + HEX"]

    def test_corrupted_payload_falls_back_instead_of_raising(self):
        config = _Config(**{color_formats.SETTING_KEY: "not json at all"})
        assert [f.name for f in color_formats.load(config) if f.enabled] == ["RGB + HEX"]


class TestDialog:
    """管理窗口：勾选、拖动换位都要如实反映到 entries()。"""

    @pytest.fixture
    def dialog(self, qapp):
        from ui.settings_ui.color_format_dialog import ColorFormatDialog

        instance = ColorFormatDialog(color_formats.default_formats())
        instance.show()
        qapp.processEvents()
        yield instance
        instance.close()
        instance.deleteLater()

    def test_checking_a_row_enables_that_format(self, dialog):
        rows = {row.format.name: row for row in dialog._list.rows()}
        rows["CSS rgb()"].check.setChecked(True)
        assert [f.name for f in dialog.entries() if f.enabled] == ["RGB + HEX", "CSS rgb()"]

    def test_dragging_a_row_to_the_top_makes_it_the_copy_format(self, dialog, qapp):
        from PySide6.QtCore import QPoint

        rows = {row.format.name: row for row in dialog._list.rows()}
        target = rows["CSS hsl()"]
        target.check.setChecked(True)
        top = dialog._list.rows()[0].mapToGlobal(QPoint(0, 1)).y()
        dialog._list._drag_row(target, top)
        qapp.processEvents()

        assert dialog.entries()[0].name == "CSS hsl()"
        assert color_formats.enabled_formats(dialog.entries())[0].name == "CSS hsl()"

    def test_rows_offer_no_add_edit_or_delete(self, dialog):
        from PySide6.QtWidgets import QAbstractButton

        for row in dialog._list.rows():
            buttons = [b for b in row.findChildren(QAbstractButton) if b is not row.check]
            assert buttons == []
        assert not hasattr(dialog, "_add_format")

    def test_restore_defaults_resets_order_and_checks(self, dialog):
        dialog._fill(list(reversed(color_formats.default_formats())))
        dialog._fill(color_formats.default_formats())
        assert dialog.entries() == color_formats.default_formats()


class TestSettingsPageWiring:
    """设置页只把编辑结果存在 dialog 上，点「应用」时才落盘。"""

    def test_editor_result_is_saved_on_accept(self, qapp, tmp_path):
        from PySide6.QtCore import QSettings
        from settings.tool_settings import ToolSettingsManager
        from ui.settings_ui.dialog import SettingsDialog

        manager = ToolSettingsManager(
            qsettings=QSettings(str(tmp_path / "s.ini"), QSettings.Format.IniFormat))
        fake = SimpleNamespace(
            config_manager=manager,
            magnifier_color_formats=[
                ColorFormat("HEX", "#{hex}", enabled=True)],
        )
        SettingsDialog._save_magnifier_color_formats(fake)

        stored = json.loads(manager.get_app_setting(color_formats.SETTING_KEY))
        assert [item["name"] for item in stored if item["enabled"]] == ["HEX"]

    def test_editing_formats_counts_as_an_unsaved_change(self):
        from ui.settings_ui.dialog import SettingsDialog

        fake = SimpleNamespace(magnifier_color_formats=color_formats.default_formats())
        fake._settings_snapshot = SettingsDialog._snapshot_settings(fake)
        fake._snapshot_settings = lambda: SettingsDialog._snapshot_settings(fake)
        assert not SettingsDialog._has_unsaved_changes(fake)

        fake.magnifier_color_formats = list(reversed(color_formats.default_formats()))
        assert SettingsDialog._has_unsaved_changes(fake)
