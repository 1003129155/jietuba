# -*- coding: utf-8 -*-
"""
ThemeManager (截图主题色) 单元测试

测试全局主题管理器的色彩逻辑。
需要 QApplication（因为 QColor 需要 Qt）。
"""
import pytest
from PySide6.QtWidgets import QApplication
from PySide6.QtGui import QColor


@pytest.fixture(scope="module")
def qapp():
    app = QApplication.instance()
    if app is None:
        app = QApplication([])
    yield app


class TestThemeManager:
    """ThemeManager 单例测试"""

    @pytest.fixture(autouse=True)
    def reset_singleton(self, qapp):
        """每个测试前重置单例，避免状态污染"""
        from core.theme import ThemeManager
        ThemeManager._instance = None
        ThemeManager._config_manager = None
        ThemeManager._theme_color = None
        ThemeManager._mask_color = None
        yield

    def test_singleton(self):
        """应为单例"""
        from core.theme import ThemeManager, get_theme
        a = ThemeManager()
        b = ThemeManager()
        c = get_theme()
        assert a is b
        assert a is c

    def test_default_theme_color(self):
        """默认主题色应为 #40E0D0"""
        from core.theme import get_theme
        tm = get_theme()
        assert tm.theme_color_hex == "#40E0D0"

    def test_default_mask_color(self):
        """默认遮罩色"""
        from core.theme import get_theme
        tm = get_theme()
        mask = tm.mask_color
        assert mask.red() == 0
        assert mask.green() == 0
        assert mask.blue() == 0
        assert mask.alpha() == 120

    def test_set_theme_color(self):
        """设置主题色（无配置管理器时不会崩溃）"""
        from core.theme import get_theme
        tm = get_theme()
        tm.set_theme_color(QColor("#FF0000"))
        assert tm.theme_color_hex == "#FF0000"

    def test_set_mask_color(self):
        """设置遮罩色"""
        from core.theme import get_theme
        tm = get_theme()
        tm.set_mask_color(QColor(100, 200, 50))
        mask = tm.mask_color
        assert mask.red() == 100
        assert mask.green() == 200
        assert mask.blue() == 50
        assert mask.alpha() == 120  # Alpha 固定 120

    def test_reset_to_defaults(self):
        """重置回默认值"""
        from core.theme import get_theme
        tm = get_theme()
        tm.set_theme_color(QColor("#FF0000"))
        tm.set_mask_color(QColor(100, 100, 100))
        tm.reset_to_defaults()
        assert tm.theme_color_hex == "#40E0D0"
        assert tm.mask_color.red() == 0

    def test_theme_color_returns_copy(self):
        """theme_color 应返回副本，不影响内部状态"""
        from core.theme import get_theme
        tm = get_theme()
        c1 = tm.theme_color
        c1.setRed(255)  # 修改副本
        c2 = tm.theme_color
        assert c2.red() != 255 or tm.theme_color_hex == "#40E0D0"

    def test_init_with_mock_config(self):
        """使用 mock 配置管理器初始化"""
        from core.theme import get_theme

        class MockConfig:
            def get_app_setting(self, key):
                return {
                    "theme_color": "#FF8800",
                    "mask_color_r": 50,
                    "mask_color_g": 60,
                    "mask_color_b": 70,
                }.get(key)
            def set_app_setting(self, key, value):
                pass

        tm = get_theme()
        tm.init(MockConfig())
        assert tm.theme_color_hex == "#FF8800"
        mask = tm.mask_color
        assert mask.red() == 50
        assert mask.green() == 60
        assert mask.blue() == 70
 