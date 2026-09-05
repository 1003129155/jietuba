# -*- coding: utf-8 -*-
"""平台相关工具函数（Windows Win32 API 等）"""
# 跨模块调用的平台相关功能集中在这里，避免分散在各个模块中直接调用 Win32 API 导致的重复代码和维护困难。
import os
import ctypes

from core.logger import log_exception, T


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
        log_exception(e, T("释放工作集"))


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
    优先使用 Per-Monitor DPI Aware（PMv1，SetProcessDpiAwareness(2)），
    失败时回退到旧版 SetProcessDPIAware（System DPI Aware）。
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

PROCESS_TERMINATE = 0x0001
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000

_kernel32_ref = None


def _kernel32():
    """惰性获取 kernel32 并声明函数签名（只做一次）。

    必须显式声明 argtypes/restype：ctypes 默认按 c_int 解释返回值，
    64 位下 HANDLE 会被截断，导致 CloseHandle 失败、句柄泄漏。
    """
    global _kernel32_ref
    if _kernel32_ref is not None:
        return _kernel32_ref

    from ctypes import wintypes

    k = ctypes.windll.kernel32
    k.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    k.OpenProcess.restype = wintypes.HANDLE
    k.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
    k.TerminateProcess.restype = wintypes.BOOL
    k.CloseHandle.argtypes = [wintypes.HANDLE]
    k.CloseHandle.restype = wintypes.BOOL
    k.GetProcessTimes.argtypes = [
        wintypes.HANDLE,
        ctypes.POINTER(wintypes.FILETIME),
        ctypes.POINTER(wintypes.FILETIME),
        ctypes.POINTER(wintypes.FILETIME),
        ctypes.POINTER(wintypes.FILETIME),
    ]
    k.GetProcessTimes.restype = wintypes.BOOL
    k.QueryFullProcessImageNameW.argtypes = [
        wintypes.HANDLE,
        wintypes.DWORD,
        wintypes.LPWSTR,
        ctypes.POINTER(wintypes.DWORD),
    ]
    k.QueryFullProcessImageNameW.restype = wintypes.BOOL

    _kernel32_ref = k
    return k


def get_process_identity(pid: int):
    """返回进程的 (创建时间, 可执行文件名)；进程不存在或无权访问时返回 None。

    Windows 会回收复用 PID，所以 PID 本身不足以标识一个进程，
    (PID, 创建时间) 才是唯一标识。创建时间取自 GetProcessTimes 的
    FILETIME，拼成 64 位整数返回，可直接用于相等比较。
    """
    if pid <= 0:
        return None

    from ctypes import wintypes

    try:
        k = _kernel32()
    except Exception as e:
        log_exception(e, T("加载 kernel32"))
        return None

    handle = k.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not handle:
        return None

    try:
        creation = wintypes.FILETIME()
        exit_time = wintypes.FILETIME()
        kernel_time = wintypes.FILETIME()
        user_time = wintypes.FILETIME()
        if not k.GetProcessTimes(
            handle,
            ctypes.byref(creation),
            ctypes.byref(exit_time),
            ctypes.byref(kernel_time),
            ctypes.byref(user_time),
        ):
            return None
        create_time = (creation.dwHighDateTime << 32) | creation.dwLowDateTime

        size = wintypes.DWORD(32768)
        buffer = ctypes.create_unicode_buffer(size.value)
        if k.QueryFullProcessImageNameW(handle, 0, buffer, ctypes.byref(size)):
            image_name = os.path.basename(buffer.value)
        else:
            image_name = ""

        return create_time, image_name
    except Exception as e:
        log_exception(e, T("查询进程标识"))
        return None
    finally:
        k.CloseHandle(handle)


def terminate_process_by_pid(pid: int) -> bool:
    """终止指定 PID 的进程。成功返回 True，失败返回 False。

    本函数不校验目标身份。Windows 会回收复用 PID，调用方必须先用
    get_process_identity() 确认目标确实是预期的那个进程，否则可能误杀无关进程。
    """
    try:
        k = _kernel32()
        handle = k.OpenProcess(PROCESS_TERMINATE, False, pid)
        if not handle:
            return False
        try:
            return bool(k.TerminateProcess(handle, 0))
        finally:
            k.CloseHandle(handle)
    except Exception as e:
        log_exception(e, T("终止进程"))
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
 