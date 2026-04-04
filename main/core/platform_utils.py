# -*- coding: utf-8 -*-
"""平台相关工具函数（Windows Win32 API 等）"""
# 跨模块调用的平台相关功能集中在这里，避免分散在各个模块中直接调用 Win32 API 导致的重复代码和维护困难。
import ctypes

from core.logger import log_exception


# ──────────────────────────────────────────────
# 内存
# ──────────────────────────────────────────────

def trim_working_set():
    """释放进程工作集，降低任务管理器显示的内存占用（Windows）"""
    try:
        from ctypes import wintypes
        # 必须正确声明参数/返回类型，否则 64 位系统上句柄会被截断
        kernel32 = ctypes.windll.kernel32
        kernel32.GetCurrentProcess.restype = wintypes.HANDLE
        kernel32.SetProcessWorkingSetSize.argtypes = [
            wintypes.HANDLE, ctypes.c_ssize_t, ctypes.c_ssize_t
        ]
        kernel32.SetProcessWorkingSetSize.restype = wintypes.BOOL
        handle = kernel32.GetCurrentProcess()
        kernel32.SetProcessWorkingSetSize(handle, -1, -1)
    except Exception as e:
        log_exception(e, "释放工作集")


_trim_timer = None  # 延迟初始化，避免在 QApplication 创建前导入时崩溃


def request_trim_working_set(delay_ms: int = 1500):
    """请求释放工作集（去抖）。多次调用只执行最后一次，避免 page fault 风暴。"""
    global _trim_timer
    if _trim_timer is None:
        from PySide6.QtCore import QTimer
        _trim_timer = QTimer()
        _trim_timer.setSingleShot(True)
        _trim_timer.timeout.connect(trim_working_set)
    _trim_timer.start(delay_ms)


# ──────────────────────────────────────────────
# DPI 感知
# ──────────────────────────────────────────────

def set_dpi_awareness():
    """设置进程 DPI 感知（必须在 QApplication 创建之前调用）。
    优先使用 Per-Monitor v2（Win10 1703+），回退到旧版 SetProcessDPIAware。
    """
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)  # PROCESS_PER_MONITOR_DPI_AWARE
    except Exception as e:
        log_exception(e, "SetProcessDpiAwareness")
        try:
            ctypes.windll.user32.SetProcessDPIAware()
        except Exception as e2:
            log_exception(e2, "SetProcessDPIAware")


# ──────────────────────────────────────────────
# 任务栏
# ──────────────────────────────────────────────

def set_app_user_model_id(app_id: str = "jietuba.app"):
    """设置 AppUserModelID，确保任务栏图标正确分组（必须在 QApplication 创建之前调用）。"""
    try:
        ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID(app_id)
    except Exception as e:
        log_exception(e, "SetAppUserModelID")


# ──────────────────────────────────────────────
# 进程管理
# ──────────────────────────────────────────────

def terminate_process_by_pid(pid: int) -> bool:
    """终止指定 PID 的进程。成功返回 True，失败返回 False。"""
    try:
        PROCESS_TERMINATE = 1
        handle = ctypes.windll.kernel32.OpenProcess(PROCESS_TERMINATE, False, pid)
        if not handle:
            return False
        ctypes.windll.kernel32.TerminateProcess(handle, 0)
        ctypes.windll.kernel32.CloseHandle(handle)
        return True
    except Exception as e:
        log_exception(e, "终止进程")
        return False


# ──────────────────────────────────────────────
# 窗口捕获排除
# ──────────────────────────────────────────────

WDA_NONE             = 0x00000000
WDA_EXCLUDEFROMCAPTURE = 0x00000011  # Windows 10 2004+


def set_window_exclude_from_capture(hwnd: int, exclude: bool) -> bool:
    """设置窗口是否从屏幕截图中排除（mss/BitBlt/DXGI 均生效）。
    窗口在屏幕上仍正常显示，仅对截图不可见。
    返回 Win32 调用是否成功。
    """
    try:
        affinity = WDA_EXCLUDEFROMCAPTURE if exclude else WDA_NONE
        result = ctypes.windll.user32.SetWindowDisplayAffinity(hwnd, affinity)
        return bool(result)
    except Exception as e:
        log_exception(e, "SetWindowDisplayAffinity")
        return False


def get_last_error() -> int:
    """返回当前线程的 Win32 LastError 值（用于诊断 Win32 API 失败原因）。"""
    try:
        return ctypes.windll.kernel32.GetLastError()
    except Exception as e:
        log_exception(e, "GetLastError")
        return -1
 