# -*- coding: utf-8 -*-
"""欢迎向导剪贴板页：外观三项即改即生效，左侧预览跟着变"""
import pytest
from PySide6.QtCore import QSettings

from clipboard.ui.theme.themes import (
    PRESET_THEMES, PRESET_THEME_SWATCHES, ThemeColors, get_theme_manager,
)
from settings.tool_settings import ToolSettingsManager
from ui.welcome.page3_clipboard import _ClipboardFeatureAnimation, ClipboardHotkeyPage


def _manager(tmp_path):
    settings = QSettings(
        str(tmp_path / "welcome_clip_appearance.ini"),
        QSettings.Format.IniFormat,
    )
    return ToolSettingsManager(qsettings=settings)


@pytest.fixture
def restore_clipboard_theme():
    """主题管理器是全局单例，用例改过之后要还原，免得污染其它测试。"""
    manager = get_theme_manager()
    before = manager.get_current_theme().name
    yield manager
    manager.set_theme(before)


# ============================================================================
# 插画配色取自真实主题
# ============================================================================

class TestIllustrationPalette:

    def test_every_palette_key_maps_to_a_real_theme_field(self):
        """映射表写错字段名只会静默取不到色，这里把它钉死。"""
        colors = ThemeColors()
        for key, field in _ClipboardFeatureAnimation.PALETTE_FIELDS.items():
            assert hasattr(colors, field), (key, field)

    def test_palette_follows_the_selected_theme(self, qapp, restore_clipboard_theme):
        restore_clipboard_theme.set_theme("light")
        light = _ClipboardFeatureAnimation._palette()
        restore_clipboard_theme.set_theme("pink")
        pink = _ClipboardFeatureAnimation._palette()

        assert light["accent"] == PRESET_THEMES["light"].colors.accent_primary
        assert pink["accent"] == PRESET_THEMES["pink"].colors.accent_primary
        assert light != pink

    def test_swatch_table_covers_every_preset_theme(self):
        """色板漏一个主题，选择器上就会少一枚方块。"""
        assert set(PRESET_THEME_SWATCHES) == set(PRESET_THEMES)

    def test_every_animation_phase_paints_without_swallowed_errors(
        self, qapp, tmp_path, monkeypatch, restore_clipboard_theme
    ):
        """把整段动画逐帧画一遍，并盯住被吞掉的异常。

        paintEvent 上挂着 safe_event：绘制路径抛错不会让测试失败，只会写一条崩溃
        日志，插画照样显示、只是内容不全。所以既要真的画，还得截住 _write_crash，
        否则这类错误在测试里完全是隐形的。
        """
        from core import crash_handler

        crashes = []
        monkeypatch.setattr(
            crash_handler, "_write_crash",
            lambda title, detail="": crashes.append(title),
        )

        page = ClipboardHotkeyPage(_manager(tmp_path))
        animation = page.illus_area.animation
        try:
            for elapsed in range(0, animation.DURATION_MS, 500):
                animation.set_animation_time(elapsed)
                animation.grab()
            assert crashes == []
        finally:
            page.close()


# ============================================================================
# 页面控件
# ============================================================================

class TestClipboardAppearanceControls:

    def test_controls_are_backfilled_from_config(self, qapp, tmp_path, restore_clipboard_theme):
        manager = _manager(tmp_path)
        manager.set_clipboard_font_size(19)
        manager.set_clipboard_window_opacity(40)
        restore_clipboard_theme.set_theme("blue")

        page = ClipboardHotkeyPage(manager)
        try:
            assert page._theme_row.current() == "blue"
            assert page._font_combo.currentData() == 19
            assert page._opacity_combo.currentData() == 40
        finally:
            page.close()

    def test_picking_a_theme_updates_the_preview(self, qapp, tmp_path, restore_clipboard_theme):
        manager = _manager(tmp_path)
        page = ClipboardHotkeyPage(manager)
        try:
            page._theme_row._pick("green")
            assert page.illus_area.animation._palette()["accent"] == (
                PRESET_THEMES["green"].colors.accent_primary
            )
        finally:
            page.close()

    def test_font_size_scales_the_preview(self, qapp, tmp_path, restore_clipboard_theme):
        manager = _manager(tmp_path)
        page = ClipboardHotkeyPage(manager)
        animation = page.illus_area.animation
        try:
            page._font_combo.setCurrentIndex(page._font_combo.findData(20))
            assert animation._font_scale > 1.0
            page._font_combo.setCurrentIndex(page._font_combo.findData(15))
            assert animation._font_scale < 1.0
        finally:
            page.close()

    def test_opacity_fades_the_preview(self, qapp, tmp_path, restore_clipboard_theme):
        manager = _manager(tmp_path)
        page = ClipboardHotkeyPage(manager)
        animation = page.illus_area.animation
        try:
            page._opacity_combo.setCurrentIndex(page._opacity_combo.findData(0))
            assert animation._opacity_factor == 1.0
            page._opacity_combo.setCurrentIndex(page._opacity_combo.findData(60))
            assert animation._opacity_factor < 1.0
        finally:
            page.close()

    def test_appearance_changes_are_written_immediately(self, qapp, tmp_path, restore_clipboard_theme):
        """这三项即改即生效，所以不经过 save()——「跳过」也应保留用户的选择。"""
        manager = _manager(tmp_path)
        page = ClipboardHotkeyPage(manager)
        try:
            page._font_combo.setCurrentIndex(page._font_combo.findData(18))
            page._opacity_combo.setCurrentIndex(page._opacity_combo.findData(50))

            assert manager.get_clipboard_font_size() == 18
            assert manager.get_clipboard_window_opacity() == 50
        finally:
            page.close()

    def test_opaque_option_is_labelled_not_zero_percent(self, qapp, tmp_path, restore_clipboard_theme):
        manager = _manager(tmp_path)
        page = ClipboardHotkeyPage(manager)
        try:
            index = page._opacity_combo.findData(0)
            assert page._opacity_combo.itemText(index) != "0%"
        finally:
            page.close()
