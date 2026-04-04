# -*- coding: utf-8 -*-
"""
崩溃处理器单元测试

测试 _write_crash / install_crash_hooks 等纯逻辑。
"""
import pytest
import sys
import os
from pathlib import Path
from core.crash_handler import _write_crash, _ensure_log_dir, _LOG_DIR


class TestEnsureLogDir:
    """日志目录创建测试"""

    def test_log_dir_created(self):
        """日志目录应可以创建"""
        _ensure_log_dir()
        assert _LOG_DIR.exists()


class TestWriteCrash:
    """崩溃日志写入测试"""

    def test_write_crash_creates_file(self):
        """写入崩溃日志应创建 crash.log"""
        _write_crash("TEST", "测试崩溃信息")
        crash_file = _LOG_DIR / "crash.log"
        assert crash_file.exists()

    def test_write_crash_content(self):
        """崩溃日志应包含标签和消息"""
        _write_crash("UNIT_TEST", "这是测试消息 12345")
        crash_file = _LOG_DIR / "crash.log"
        content = crash_file.read_text(encoding="utf-8")
        assert "UNIT_TEST" in content
        assert "12345" in content

    def test_write_crash_appends(self):
        """多次写入应追加而非覆盖"""
        _write_crash("APPEND_A", "第一条")
        _write_crash("APPEND_B", "第二条")
        crash_file = _LOG_DIR / "crash.log"
        content = crash_file.read_text(encoding="utf-8")
        assert "APPEND_A" in content
        assert "APPEND_B" in content


class TestInstallCrashHooks:
    """安装崩溃钩子测试"""

    def test_install_does_not_crash(self):
        """安装钩子不应崩溃"""
        from core.crash_handler import install_crash_hooks
        # 保存原有钩子
        old_except = sys.excepthook
        try:
            install_crash_hooks()
        finally:
            # 恢复原有钩子，避免影响其他测试
            sys.excepthook = old_except
 