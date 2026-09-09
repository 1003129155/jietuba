# -*- coding: utf-8 -*-
"""
崩溃处理器单元测试

测试 _write_crash / install_crash_hooks 等纯逻辑。
"""
import sys
import threading
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
        """安装钩子不应崩溃。

        四个钩子都要还原，不能只还原 sys.excepthook。漏掉的那三个里最要命的是
        faulthandler：install_crash_hooks 会把它重定向到应用自己的日志文件，
        本测试之后整场会话的原生崩溃堆栈就都写进那个文件，而不是 stderr——
        进程猝死时 CI 日志上只剩一行 exit code，堆栈躺在谁也不会看的地方。
        """
        import faulthandler
        from core.crash_handler import install_crash_hooks

        old_except = sys.excepthook
        old_thread_except = threading.excepthook
        old_unraisable = sys.unraisablehook
        was_enabled = faulthandler.is_enabled()
        try:
            install_crash_hooks()
        finally:
            sys.excepthook = old_except
            threading.excepthook = old_thread_except
            sys.unraisablehook = old_unraisable
            # enable() 不带 file 就回到默认的 stderr
            faulthandler.enable() if was_enabled else faulthandler.disable()
 