# -*- coding: utf-8 -*-
"""
剪贴板主题系统单元测试

测试 ThemeColors / Theme / ThemeStyleGenerator 的纯逻辑部分。
"""
import pytest
from clipboard.themes import ThemeColors, Theme, THEME_LIGHT, THEME_DARK, THEME_BLUE
from clipboard.theme_styles import ThemeStyleGenerator, generate_all_styles


# ==================== ThemeColors 测试 ====================

class TestThemeColors:
    """ThemeColors 数据类测试"""

    def test_default_values(self):
        """默认颜色值应该合法"""
        colors = ThemeColors()
        assert colors.bg_primary == "#FFFFFF"
        assert colors.text_primary == "#333333"
        assert colors.accent_primary == "#1976D2"

    def test_to_dict(self):
        """to_dict 应返回包含所有字段的字典"""
        colors = ThemeColors()
        d = colors.to_dict()
        assert isinstance(d, dict)
        assert "bg_primary" in d
        assert "text_primary" in d
        assert "accent_primary" in d
        assert d["bg_primary"] == "#FFFFFF"

    def test_from_dict_roundtrip(self):
        """从字典创建的颜色应与原始值一致"""
        original = ThemeColors(bg_primary="#123456", text_primary="#ABCDEF")
        d = original.to_dict()
        restored = ThemeColors.from_dict(d)
        assert restored.bg_primary == "#123456"
        assert restored.text_primary == "#ABCDEF"

    def test_custom_colors(self):
        """自定义颜色应正确存储"""
        colors = ThemeColors(bg_primary="#000000", error="#FF0000")
        assert colors.bg_primary == "#000000"
        assert colors.error == "#FF0000"


# ==================== Theme 测试 ====================

class TestTheme:
    """Theme 类测试"""

    def test_theme_creation(self):
        """创建主题应正确设置属性"""
        theme = Theme(
            name="test",
            display_name="测试主题",
            colors=ThemeColors(),
            description="测试描述"
        )
        assert theme.name == "test"
        assert theme.display_name == "测试主题"
        assert theme.description == "测试描述"

    def test_theme_to_dict(self):
        """主题序列化为字典"""
        theme = Theme(name="t", display_name="T", colors=ThemeColors())
        d = theme.to_dict()
        assert d["name"] == "t"
        assert d["display_name"] == "T"
        assert "colors" in d

    def test_theme_from_dict_roundtrip(self):
        """主题字典序列化往返一致"""
        original = Theme(
            name="custom",
            display_name="自定义",
            colors=ThemeColors(bg_primary="#111111"),
            description="desc"
        )
        d = original.to_dict()
        restored = Theme.from_dict(d)
        assert restored.name == "custom"
        assert restored.colors.bg_primary == "#111111"

    def test_preset_themes_exist(self):
        """预设主题应存在且名称正确"""
        assert THEME_LIGHT.name == "light"
        assert THEME_DARK.name == "dark"
        assert THEME_BLUE.name == "blue"


# ==================== ThemeStyleGenerator 测试 ====================

class TestThemeStyleGenerator:
    """样式生成器测试"""

    @pytest.fixture
    def generator(self):
        return ThemeStyleGenerator(THEME_LIGHT)

    def test_hex_to_rgb(self, generator):
        """十六进制颜色转 RGB 字符串"""
        assert generator._hex_to_rgb("#FFFFFF") == "255, 255, 255"
        assert generator._hex_to_rgb("#000000") == "0, 0, 0"
        assert generator._hex_to_rgb("#FF8000") == "255, 128, 0"

    def test_hex_to_rgb_short(self, generator):
        """三位十六进制颜色"""
        assert generator._hex_to_rgb("#FFF") == "255, 255, 255"
        assert generator._hex_to_rgb("#000") == "0, 0, 0"

    def test_hex_to_rgb_no_hash(self, generator):
        """无 # 前缀的十六进制颜色"""
        assert generator._hex_to_rgb("FF0000") == "255, 0, 0"

    def test_generate_window_style(self, generator):
        """窗口样式应包含关键选择器"""
        style = generator.generate_window_style()
        assert "QFrame#mainContainer" in style
        assert "QToolTip" in style
        assert "background" in style

    def test_generate_window_style_with_opacity(self, generator):
        """带透明度的窗口样式"""
        style = generator.generate_window_style(opacity=50)
        assert "rgba" in style

    def test_generate_button_styles(self, generator):
        """按钮样式变体"""
        normal = generator.generate_button_style("normal")
        primary = generator.generate_button_style("primary")
        danger = generator.generate_button_style("danger")

        assert "QPushButton" in normal
        assert "QPushButton" in primary
        assert "QPushButton" in danger
        # primary 按钮应有强调色
        assert THEME_LIGHT.colors.accent_primary in primary

    def test_generate_list_widget_style(self, generator):
        """列表控件样式"""
        style = generator.generate_list_widget_style()
        assert "QListWidget" in style

    def test_generate_search_bar_style(self, generator):
        """搜索栏样式"""
        style = generator.generate_search_bar_style()
        assert "border-top" in style

    def test_generate_input_style(self, generator):
        """输入框样式"""
        style = generator.generate_input_style()
        assert "QLineEdit" in style
        assert "QTextEdit" in style

    def test_generate_combobox_style(self, generator):
        """下拉框样式"""
        style = generator.generate_combobox_style()
        assert "QComboBox" in style

    def test_generate_dialog_style(self, generator):
        """对话框样式"""
        style = generator.generate_dialog_style()
        assert "QDialog" in style

    def test_generate_preview_popup_style(self, generator):
        """预览弹窗样式"""
        style = generator.generate_preview_popup_style()
        assert "PreviewPopup" in style


# ==================== generate_all_styles 测试 ====================

class TestGenerateAllStyles:
    """generate_all_styles 函数测试"""

    def test_returns_all_keys(self):
        """应返回所有预期的样式键"""
        styles = generate_all_styles(THEME_LIGHT)
        expected_keys = {
            "window", "list_widget", "search_bar",
            "button_normal", "button_primary", "button_danger",
            "input", "combobox", "dialog", "manage_dialog", "preview_popup",
        }
        assert set(styles.keys()) == expected_keys

    def test_all_values_are_strings(self):
        """所有样式值应为字符串"""
        styles = generate_all_styles(THEME_LIGHT)
        for key, value in styles.items():
            assert isinstance(value, str), f"styles['{key}'] 不是字符串"

    def test_dark_theme_styles(self):
        """深色主题也应能正常生成样式"""
        styles = generate_all_styles(THEME_DARK)
        assert len(styles) == 11
        # 深色主题的背景色应出现在样式中
        assert THEME_DARK.colors.bg_primary in styles["dialog"]

    def test_opacity_parameter(self):
        """透明度参数应影响样式内容"""
        styles_opaque = generate_all_styles(THEME_LIGHT, opacity=0)
        styles_transparent = generate_all_styles(THEME_LIGHT, opacity=50)
        # 两次生成的窗口样式应不同（因为 alpha 不同）
        assert styles_opaque["window"] != styles_transparent["window"]
 