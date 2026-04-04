# -*- coding: utf-8 -*-
"""
核心常量和工具函数测试
"""
import pytest


class TestConstants:
    """constants.py 常量测试"""

    def test_font_families_defined(self):
        """字体族常量应已定义且非空"""
        from core.constants import CSS_FONT_FAMILY, CSS_FONT_FAMILY_UI, DEFAULT_FONT_FAMILY
        assert CSS_FONT_FAMILY
        assert CSS_FONT_FAMILY_UI
        assert DEFAULT_FONT_FAMILY
        assert "Microsoft YaHei" in CSS_FONT_FAMILY

    def test_default_font_family_is_string(self):
        """DEFAULT_FONT_FAMILY 应为普通字符串"""
        from core.constants import DEFAULT_FONT_FAMILY
        assert isinstance(DEFAULT_FONT_FAMILY, str)
        assert '"' not in DEFAULT_FONT_FAMILY  # 不含引号


class TestLogger:
    """logger 模块可导入性测试"""

    def test_imports(self):
        """核心日志函数应可导入"""
        from core.logger import log_debug, log_info, log_warning, log_error, log_exception
        assert callable(log_debug)
        assert callable(log_info)
        assert callable(log_warning)
        assert callable(log_error)
        assert callable(log_exception)
 