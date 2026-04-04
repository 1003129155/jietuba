"""
全局崩溃捕获模块 - 统一异常捕获

覆盖四层异常来源：
1. sys.excepthook          — 主线程未捕获的 Python 异常
2. threading.excepthook    — 子线程未捕获的 Python 异常（Python 3.8+）
3. sys.unraisablehook      — 析构函数 / __del__ / 回调中被静默吞掉的异常
4. faulthandler            — C 层面的 segfault / abort（ctypes、Rust 库等崩溃）

"""

import sys
import threading
import traceback
import functools
from pathlib import Path
from datetime import datetime


# ============================================================================
# 日志目录（与 logger.py 保持一致）
# ============================================================================
_LOG_DIR = Path.home() / "AppData" / "Local" / "Jietuba" / "Logs"
_CRASH_FILE = "crash.log"

# faulthandler 输出文件句柄（模块级持有，防止被 GC）
_faulthandler_fp = None


def _ensure_log_dir():
    """确保日志目录存在"""
    try:
        _LOG_DIR.mkdir(parents=True, exist_ok=True)
    except Exception:
        pass


def _write_crash(tag: str, msg: str):
    """
    将崩溃信息同时输出到：
      1. 终端 stdout（开发时可见）
      2. crash.log 独立文件（即使 Logger 没初始化也能留痕）
      3. Logger 系统日志（如果已初始化）
    """
    # --- 终端 ---
    text = (
        f"\n{'='*60}\n"
        f"❌ {tag}:\n"
        f"{msg}"
        f"{'='*60}\n"
    )
    try:
        sys.__stderr__.write(text)      # 用 __stderr__ 绕过可能被重定向的 stderr
        sys.__stderr__.flush()
    except Exception:
        pass

    # --- crash.log（独立于 Logger，最小依赖） ---
    _ensure_log_dir()
    try:
        crash_path = _LOG_DIR / _CRASH_FILE
        with open(crash_path, "a", encoding="utf-8") as f:
            f.write(f"[{datetime.now():%Y-%m-%d %H:%M:%S}] {tag}\n{msg}{'='*60}\n\n")
    except Exception:
        pass

    # --- Logger 系统日志（如果可用） ---
    try:
        from core.logger import get_logger
        logger = get_logger()
        if logger and logger._ready:
            logger.error(f"{tag}:\n{msg}", "CRASH")
    except Exception:
        pass


# ============================================================================
# 四层钩子
# ============================================================================

def _excepthook(exc_type, exc_value, exc_tb):
    """① 主线程未捕获异常"""
    error_msg = ''.join(traceback.format_exception(exc_type, exc_value, exc_tb))
    _write_crash("未处理的异常（主线程）", error_msg)


def _threading_excepthook(args):
    """② 子线程未捕获异常（Python 3.8+）
    
    threading.Thread 里未捕获的异常默认只打印到 stderr 然后线程静默退出，
    主线程完全不知道。
    """
    error_msg = ''.join(traceback.format_exception(
        args.exc_type, args.exc_value, args.exc_traceback
    ))
    thread_name = args.thread.name if args.thread else "Unknown"
    _write_crash(f"未处理的异常（子线程: {thread_name}）", error_msg)


def _unraisablehook(unraisable):
    """③ 不可抛出异常（__del__ / 回调 / finalizer 中的异常）
    
    PyQt 的信号回调、对象析构中的异常会被 Python 静默吞掉。
    """
    exc = unraisable.exc_value
    if exc is None:
        return
    error_msg = ''.join(traceback.format_exception(type(exc), exc, exc.__traceback__))
    obj_info = f" (对象: {unraisable.object!r})" if unraisable.object else ""
    _write_crash(f"不可抛出异常{obj_info}", error_msg)


def _install_faulthandler():
    """④ C 层面 segfault / abort 堆栈追踪
    
    ctypes 调用出错、Rust 库崩溃等导致的进程闪退，
    faulthandler 会在进程死亡前把 C 堆栈写入文件。
    这是最后的防线。
    """
    global _faulthandler_fp
    import faulthandler

    _ensure_log_dir()
    try:
        _faulthandler_fp = open(
            _LOG_DIR / "faulthandler.log", "a", encoding="utf-8"
        )
        # 写入分隔线，方便区分不同次崩溃
        _faulthandler_fp.write(f"\n--- session {datetime.now():%Y-%m-%d %H:%M:%S} ---\n")
        _faulthandler_fp.flush()
        faulthandler.enable(file=_faulthandler_fp)
    except Exception:
        # 至少输出到 stderr
        faulthandler.enable()


# ============================================================================
# 公开 API
# ============================================================================

def install_crash_hooks():
    """
    一次性安装所有崩溃捕获钩子。
    
    应在程序最早期调用（main_app.py 顶部，任何 Qt 导入之前）。
    重复调用是安全的（幂等）。
    """
    sys.excepthook = _excepthook

    # threading.excepthook 需要 Python 3.8+
    if hasattr(threading, 'excepthook'):
        threading.excepthook = _threading_excepthook

    sys.unraisablehook = _unraisablehook

    _install_faulthandler()


# ============================================================================
# Qt 事件安全装饰器
# ============================================================================

def safe_event(func):
    """
    装饰 QWidget 的事件处理函数（xxxEvent / eventFilter），两层保护：
    
    1. 根源防护：如果对象有 _is_closed 标记且为 True，直接短路返回，
       彻底阻止事件打进正在/已经 cleanup 的对象。
    2. 兜底捕获：即使没有 _is_closed 或其他原因异常，也不会被 Qt C++ 静默吞掉。
    
    注意：eventFilter 必须返回 bool，装饰器在短路/异常时返回 False。
    
    用法：
        class PinWindow(QWidget):
            @safe_event
            def wheelEvent(self, event):
                ...
            @safe_event
            def eventFilter(self, obj, event):
                ...
                return super().eventFilter(obj, event)
    """
    # eventFilter 要求返回 bool，短路/异常时必须返回 False 而不是 None
    _is_filter = (func.__name__ == 'eventFilter')

    @functools.wraps(func)
    def wrapper(self, *args, **kwargs):
        # 根源防护：对象正在关闭，不再处理任何事件
        if getattr(self, '_is_closed', False):
            return False if _is_filter else None
        try:
            return func(self, *args, **kwargs)
        except Exception:
            cls_name = type(self).__name__
            error_msg = traceback.format_exc()
            _write_crash(
                f"Qt事件异常 {cls_name}.{func.__name__}",
                error_msg,
            )
            return False if _is_filter else None
    return wrapper
 