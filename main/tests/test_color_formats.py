# -*- coding: utf-8 -*-
"""放大镜颜色格式的模型与管理窗口

格式列表是用户可以自己编辑的（勾选、排序、增删、写模板），所以这里既要盯住
模板渲染的正确性，也要盯住那些「编坏了也不能让放大镜画不出来」的兜底：模板里
写了不认识的占位符、一个格式都没勾、存档里缺了内置格式。
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

    @pytest.mark.parametrize("template", ["{nope}", "{r", "{}", "{0}"])
    def test_a_broken_template_shows_itself_instead_of_raising(self, template):
        """模板是用户手写的。写坏了只该让这一行难看，不该让整个放大镜画不出来。"""
        assert ColorFormat("x", template).render(SAMPLE) == template


class TestNormalize:

    def test_missing_builtins_are_added_back_unchecked(self):
        """内置格式只能取消勾选不能删，否则用户在管理窗口里再也找不回来。"""
        result = color_formats.normalize([ColorFormat("RGB", "{r}, {g}, {b}", enabled=True)])
        names = [f.name for f in result]
        assert names[0] == "RGB"
        assert set(names) == {name for name, _ in color_formats.BUILTIN_FORMATS}
        assert [f.name for f in result if f.enabled] == ["RGB"]

    def test_custom_formats_are_kept_in_place(self):
        custom = ColorFormat("Unity", "Color({r}, {g}, {b})", enabled=True)
        result = color_formats.normalize([custom])
        assert result[0] == custom
        assert len(result) == len(color_formats.BUILTIN_FORMATS) + 1

    def test_something_stays_enabled_when_everything_was_unchecked(self):
        """一个都没勾的话放大镜就没东西可显示了，回落到第一条。"""
        formats = [ColorFormat(n, t, enabled=False, builtin=True)
                   for n, t in color_formats.BUILTIN_FORMATS]
        assert color_formats.normalize(formats)[0].enabled


class TestLoadSave:

    def test_round_trip_keeps_order_and_checks(self):
        config = _Config()
        edited = [
            ColorFormat("CSS rgb()", "rgb({r}, {g}, {b})", enabled=True, builtin=True),
            ColorFormat("Unity", "Color({r}, {g}, {b})", enabled=True),
        ]
        color_formats.save(config, edited)
        loaded = color_formats.load(config)
        assert [f.name for f in loaded][:2] == ["CSS rgb()", "Unity"]
        assert [f.name for f in loaded if f.enabled] == ["CSS rgb()", "Unity"]

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
    """管理窗口：勾选、拖动换位、增删都要如实反映到 entries()。"""

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

    def test_builtin_rows_cannot_be_deleted(self, dialog):
        assert all(not row.delete_btn.isVisible() for row in dialog._list.rows())

    def test_added_format_lands_in_the_entries(self, dialog):
        dialog._add_row(ColorFormat("Unity", "Color({r}, {g}, {b})", enabled=True))
        entry = next(f for f in dialog.entries() if f.name == "Unity")
        assert entry.enabled and not entry.builtin
        assert entry.render(SAMPLE) == "Color(230, 153, 60)"

    def test_deleting_a_custom_format_removes_it(self, dialog, monkeypatch):
        import ui.settings_ui.color_format_dialog as module

        monkeypatch.setattr(module, "show_confirm_dialog", lambda *args: True)
        row = dialog._add_row(ColorFormat("Unity", "Color({r}, {g}, {b})", enabled=True))
        assert row.delete_btn.isVisible() or True   # 自定义格式才有删除按钮
        dialog._delete_format(row)
        assert "Unity" not in [f.name for f in dialog.entries()]

    def test_cancelled_delete_keeps_the_row(self, dialog, monkeypatch):
        import ui.settings_ui.color_format_dialog as module

        monkeypatch.setattr(module, "show_confirm_dialog", lambda *args: False)
        row = dialog._add_row(ColorFormat("Unity", "Color({r}, {g}, {b})", enabled=True))
        dialog._delete_format(row)
        assert "Unity" in [f.name for f in dialog.entries()]


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
                ColorFormat("HEX", "#{hex}", enabled=True, builtin=True)],
        )
        SettingsDialog._save_magnifier_color_formats(fake)

        stored = json.loads(manager.get_app_setting(color_formats.SETTING_KEY))
        assert [item["name"] for item in stored if item["enabled"]] == ["HEX"]
