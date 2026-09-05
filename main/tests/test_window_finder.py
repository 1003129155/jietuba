# -*- coding: utf-8 -*-
"""
WindowFinder 单元测试

覆盖 main/capture/window_finder.py 中不依赖真实 Windows 桌面枚举的纯逻辑部分：
- find_window_at_point 的 Z-order 命中测试与降级矩形
- set_screen_offset 的偏移记录
- clear() 状态复位
- _get_virtual_desktop_rect 的降级链（ctypes 失败 -> Qt primaryScreen -> 硬编码默认值）
- is_smart_selection_available 的可用性开关

find_windows()（EnumWindows 真实枚举）和 get_window_rect_no_shadow()（DWM API）
依赖真实 win32 会话，在无桌面的 CI runner 上不可测，不在本文件覆盖范围内。
"""
import pytest
from unittest.mock import patch, MagicMock

import capture.window_finder as window_finder_module
from capture.window_finder import WindowFinder, is_smart_selection_available


pytestmark = pytest.mark.skipif(
    not window_finder_module.WINDOWS_API_AVAILABLE,
    reason="win32gui 不可用，WindowFinder 在该环境下无法实例化",
)


class TestWindowFinderInit:
    def test_default_offset_is_zero(self):
        finder = WindowFinder()
        assert finder.screen_offset_x == 0
        assert finder.screen_offset_y == 0
        assert finder.windows == []

    def test_custom_offset(self):
        finder = WindowFinder(screen_offset_x=100, screen_offset_y=-50)
        assert finder.screen_offset_x == 100
        assert finder.screen_offset_y == -50

    def test_raises_when_windows_api_unavailable(self):
        with patch.object(window_finder_module, "WINDOWS_API_AVAILABLE", False):
            with pytest.raises(RuntimeError):
                WindowFinder()


class TestSetScreenOffset:
    def test_updates_offsets(self):
        finder = WindowFinder()
        finder.set_screen_offset(200, 300)
        assert finder.screen_offset_x == 200
        assert finder.screen_offset_y == 300


class TestFindWindowAtPoint:
    def test_returns_topmost_window_containing_point(self):
        """windows 列表按 Z-order 排列，第一个命中的矩形应被返回"""
        finder = WindowFinder()
        # 两个重叠窗口：hwnd 1 在最上层（列表首位），hwnd 2 在其下
        finder.windows = [
            (1, [0, 0, 500, 500], "Top Window"),
            (2, [0, 0, 1000, 1000], "Bottom Window"),
        ]
        result = finder.find_window_at_point(100, 100)
        assert result == [0, 0, 500, 500]

    def test_skips_non_containing_window_and_finds_next(self):
        finder = WindowFinder()
        finder.windows = [
            (1, [0, 0, 100, 100], "Top Left"),
            (2, [200, 200, 800, 800], "Bottom Right"),
        ]
        # 点(300, 300) 不在第一个窗口内，但在第二个窗口内
        result = finder.find_window_at_point(300, 300)
        assert result == [200, 200, 800, 800]

    def test_point_on_boundary_is_inclusive(self):
        """边界值应被视为命中（<=判断）"""
        finder = WindowFinder()
        finder.windows = [(1, [10, 10, 110, 110], "Box")]
        assert finder.find_window_at_point(10, 10) == [10, 10, 110, 110]
        assert finder.find_window_at_point(110, 110) == [10, 10, 110, 110]

    def test_no_match_returns_fallback_rect(self):
        finder = WindowFinder()
        finder.windows = [(1, [0, 0, 50, 50], "Small Window")]
        fallback = [0, 0, 1920, 1080]
        result = finder.find_window_at_point(9999, 9999, fallback_rect=fallback)
        assert result == fallback

    def test_no_match_no_fallback_returns_virtual_desktop_rect(self):
        finder = WindowFinder()
        finder.windows = []
        with patch.object(
            finder, "_get_virtual_desktop_rect", return_value=[0, 0, 3840, 1080]
        ) as mock_vd:
            result = finder.find_window_at_point(500, 500)
        mock_vd.assert_called_once()
        assert result == [0, 0, 3840, 1080]

    def test_empty_windows_list_falls_back(self):
        finder = WindowFinder()
        finder.windows = []
        fallback = [1, 2, 3, 4]
        assert finder.find_window_at_point(0, 0, fallback_rect=fallback) == fallback


class TestGetVirtualDesktopRect:
    def test_uses_system_metrics_when_available(self):
        finder = WindowFinder()
        fake_user32 = MagicMock()
        # SM_XVIRTUALSCREEN=76, SM_YVIRTUALSCREEN=77, SM_CXVIRTUALSCREEN=78, SM_CYVIRTUALSCREEN=79
        fake_user32.GetSystemMetrics.side_effect = lambda idx: {
            76: -1920,
            77: 0,
            78: 3840,
            79: 1080,
        }[idx]

        with patch.object(window_finder_module.ctypes, "windll") as mock_windll:
            mock_windll.user32 = fake_user32
            result = finder._get_virtual_desktop_rect()

        assert result == [-1920, 0, -1920 + 3840, 0 + 1080]

    def test_falls_back_to_hardcoded_default_on_total_failure(self):
        """ctypes 和 Qt primaryScreen 都失败时，应返回硬编码的 1920x1080 默认值"""
        finder = WindowFinder()

        with patch.object(window_finder_module.ctypes, "windll") as mock_windll:
            mock_windll.user32.GetSystemMetrics.side_effect = OSError("no display")
            with patch("PySide6.QtGui.QGuiApplication.primaryScreen", return_value=None):
                result = finder._get_virtual_desktop_rect()

        assert result == [0, 0, 1920, 1080]


class TestClear:
    def test_clear_resets_windows_list(self):
        finder = WindowFinder()
        finder.windows = [(1, [0, 0, 10, 10], "Something")]
        finder.clear()
        assert finder.windows == []


class TestIsSmartSelectionAvailable:
    def test_reflects_module_flag_true(self):
        with patch.object(window_finder_module, "WINDOWS_API_AVAILABLE", True):
            assert is_smart_selection_available() is True

    def test_reflects_module_flag_false(self):
        with patch.object(window_finder_module, "WINDOWS_API_AVAILABLE", False):
            assert is_smart_selection_available() is False
