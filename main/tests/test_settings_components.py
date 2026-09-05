# -*- coding: utf-8 -*-
"""
设置页共享主题工具测试（ui/settings_ui/components.py）

components.py 被六个设置页共用，其中一组 theme_* 函数负责按当前明暗主题
产出颜色和样式串。它们是纯函数——唯一的外部依赖是 get_ui_theme()——
但此前没有任何覆盖：主题切换后颜色取错、样式串少一个分号导致整段 CSS 失效，
这类问题只能靠肉眼看界面才能发现。

隔离方式：这些函数在模块顶部 import get_ui_theme，因此打桩目标是
ui.settings_ui.components.get_ui_theme 本身，不必启动 QApplication，
也不必真的切换应用主题。
"""
from types import SimpleNamespace

import pytest

from ui.settings_ui import components


@pytest.fixture
def theme(monkeypatch):
    """返回一个可现场改写 is_dark / tokens 的假主题"""
    fake = SimpleNamespace(
        is_dark=False,
        tokens=SimpleNamespace(text="#111111", text_muted="#888888"),
    )
    monkeypatch.setattr(components, "get_ui_theme", lambda: fake)
    return fake


# components 里成对取值的派生函数，以及它们的 (light, dark) 期望值
DERIVED_COLORS = {
    "theme_surface_color": ("rgba(239, 244, 250, 0.91)", "#202124"),
    "theme_sidebar_color": ("rgba(246, 249, 252, 0.54)", "#25272B"),
    "theme_border_color": ("rgba(255, 255, 255, 0.76)", "rgba(255, 255, 255, 0.08)"),
    "theme_input_background": ("rgba(255, 255, 255, 0.78)", "#2B2D31"),
    "theme_popup_background": ("#FFFFFF", "#2A2C30"),
    "theme_popup_hover_background": ("#EAF2FA", "#36393F"),
}


class TestThemeColor:

    def test_light_theme_takes_the_first_value(self, theme):
        theme.is_dark = False
        assert components.theme_color("light-value", "dark-value") == "light-value"

    def test_dark_theme_takes_the_second_value(self, theme):
        theme.is_dark = True
        assert components.theme_color("light-value", "dark-value") == "dark-value"

    def test_selection_follows_the_theme_on_every_call(self, theme):
        """主题是运行期可切换的，函数不能缓存首次读到的值"""
        theme.is_dark = False
        assert components.theme_color("a", "b") == "a"
        theme.is_dark = True
        assert components.theme_color("a", "b") == "b"
        theme.is_dark = False
        assert components.theme_color("a", "b") == "a"


class TestDerivedColors:

    def test_each_helper_returns_its_light_or_dark_token(self, theme):
        for name, (light, dark) in DERIVED_COLORS.items():
            func = getattr(components, name)
            theme.is_dark = False
            assert func() == light, name
            theme.is_dark = True
            assert func() == dark, name

    def test_light_and_dark_variants_always_differ(self, theme):
        for name, (light, dark) in DERIVED_COLORS.items():
            assert light != dark, name


class TestThemeTextStyle:

    def test_default_style_uses_the_theme_text_colour(self, theme):
        assert components.theme_text_style() == (
            "font-size: 13px; color: #111111; background: transparent;")

    def test_font_size_is_honoured(self, theme):
        for size in (9, 13, 20):
            assert f"font-size: {size}px;" in components.theme_text_style(font_size=size)

    def test_bold_appends_a_font_weight(self, theme):
        assert components.theme_text_style(bold=True) == (
            "font-size: 13px; color: #111111; background: transparent; font-weight: 600;")

    def test_extra_css_is_appended_after_a_single_space(self, theme):
        assert components.theme_text_style(extra="margin-left: 4px;") == (
            "font-size: 13px; color: #111111; background: transparent; margin-left: 4px;")

    def test_whitespace_only_extra_adds_nothing(self, theme):
        for extra in ("", "   ", "\t\n"):
            assert components.theme_text_style(extra=extra) == components.theme_text_style()

    def test_extra_is_trimmed_so_no_double_space_appears(self, theme):
        style = components.theme_text_style(extra="   padding: 2px;   ")
        assert "transparent; padding: 2px;" in style
        assert "  " not in style

    def test_bold_and_extra_combine_in_a_stable_order(self, theme):
        style = components.theme_text_style(bold=True, extra="padding: 2px;")
        assert style.index("font-weight: 600;") < style.index("padding: 2px;")

    def test_dark_theme_switches_the_text_colour(self, theme):
        theme.tokens = SimpleNamespace(text="#EEEEEE", text_muted="#AAAAAA")
        assert "color: #EEEEEE;" in components.theme_text_style()


class TestThemeCaptionStyle:

    def test_caption_uses_the_muted_colour_and_a_smaller_default_size(self, theme):
        assert components.theme_caption_style() == (
            "font-size: 12px; color: #888888; background: transparent;")

    def test_caption_never_goes_bold(self, theme):
        """说明文字没有 bold 参数，样式串里也不该出现字重"""
        assert "font-weight" not in components.theme_caption_style()
        assert "font-weight" not in components.theme_caption_style(font_size=20)

    def test_extra_css_is_appended(self, theme):
        assert components.theme_caption_style(extra="margin: 0;") == (
            "font-size: 12px; color: #888888; background: transparent; margin: 0;")

    def test_whitespace_only_extra_adds_nothing(self, theme):
        for extra in ("", "  ", "\n"):
            assert components.theme_caption_style(extra=extra) == (
                components.theme_caption_style())

    def test_caption_and_body_text_use_different_colours(self, theme):
        assert components.theme_text_style() != components.theme_caption_style(font_size=13)
