# -*- coding: utf-8 -*-
"""前台窗口跟踪：记住最后一个不属于本进程的前台窗口，作为粘贴目标。

粘贴是"把焦点还给目标窗口 + 模拟 Ctrl+V"。keybd_event 没有收件人，按键落到谁
身上取决于那一刻谁持有焦点，所以必须先知道该把焦点还给谁。

窗口失焦即隐藏时，show 时取一次样就够——记录和使用之间用户没有机会切换窗口。
窗口一旦可以常驻（粘贴后不关闭），中途焦点会反复变化，必须持续取样。

取样要排除拾取窗口自己：用户点在它上面时前台就是它，此刻记录等于把粘贴目标改
成自己，Ctrl+V 会落进搜索框。但只排除它一个——本应用的其他窗口（内容编辑、截图
标注、翻译）都是合法的粘贴目标，按进程排除会把它们一起误伤。同样排除任务栏、
桌面，以及拿不到键盘焦点的窗口——它们收不到模拟按键。
取样不合格时保留上一次的值，不回退到当前前台。
"""

import ctypes
from ctypes import wintypes
from typing import Callable, Optional

from core.logger import T, log_debug, log_exception

_user32 = ctypes.windll.user32
_kernel32 = ctypes.windll.kernel32

# 64 位下句柄按默认 int 返回会被截断，必须声明类型
_user32.GetForegroundWindow.restype = wintypes.HWND
_user32.IsWindow.argtypes = [wintypes.HWND]
_user32.IsWindow.restype = wintypes.BOOL
_user32.IsWindowVisible.argtypes = [wintypes.HWND]
_user32.IsWindowVisible.restype = wintypes.BOOL
_user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
_user32.GetWindowThreadProcessId.restype = wintypes.DWORD
_user32.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
_user32.GetClassNameW.restype = ctypes.c_int
_user32.GetWindowLongW.argtypes = [wintypes.HWND, ctypes.c_int]
_user32.GetWindowLongW.restype = wintypes.LONG

GWL_EXSTYLE = -20
WS_EX_NOACTIVATE = 0x08000000

# 任务栏、通知区域溢出面板、桌面。粘进这些窗口没有意义。
_SHELL_CLASSES = frozenset({
    "Shell_TrayWnd",
    "NotifyIconOverflowWindow",
    "TrayNotifyWnd",
    "Progman",
    "WorkerW",
})

# 取样间隔。用户在目标窗口里停留不足一个间隔就切回来的情况会漏采，所以取值
# 需要短于一次"切过去点一下再切回来"的最短耗时。
_SAMPLE_INTERVAL_MS = 200


def get_foreground_hwnd() -> Optional[int]:
    """当前前台窗口句柄；取不到返回 None。"""
    try:
        return _user32.GetForegroundWindow() or None
    except Exception as e:
        log_exception(e, T("读取前台窗口"))
        return None


def is_alive(hwnd: Optional[int]) -> bool:
    """句柄是否仍指向一个存在且可见的窗口。"""
    if not hwnd:
        return False
    try:
        return bool(_user32.IsWindow(hwnd)) and bool(_user32.IsWindowVisible(hwnd))
    except Exception as e:
        log_exception(e, T("校验窗口句柄"))
        return False


def get_window_pid(hwnd: int) -> Optional[int]:
    """窗口所属进程 ID；取不到返回 None。"""
    try:
        pid = wintypes.DWORD()
        _user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        return pid.value or None
    except Exception as e:
        log_exception(e, T("读取窗口进程"))
        return None


def get_window_class(hwnd: int) -> str:
    """窗口类名；取不到返回空串。"""
    try:
        buffer = ctypes.create_unicode_buffer(256)
        if _user32.GetClassNameW(hwnd, buffer, len(buffer)) <= 0:
            return ""
        return buffer.value
    except Exception as e:
        log_exception(e, T("读取窗口类名"))
        return ""


def can_take_focus(hwnd: int) -> bool:
    """窗口能否接受键盘焦点。拿不到焦点的窗口收不到模拟按键。"""
    try:
        return not (_user32.GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_NOACTIVATE)
    except Exception as e:
        log_exception(e, T("读取窗口扩展样式"))
        return True


def get_current_pid() -> int:
    return _kernel32.GetCurrentProcessId()


class ForegroundWindowTracker:
    """周期性取样前台窗口，保留最后一个可作为粘贴目标的句柄。

    QTimer 延迟到 start() 才创建，这样控制器可以在没有 QApplication 的环境里
    构造（测试直接调 sample()）。
    """

    def __init__(self, interval_ms: int = _SAMPLE_INTERVAL_MS):
        self._interval_ms = interval_ms
        self._target_hwnd: Optional[int] = None
        self._is_excluded: Optional[Callable[[int], bool]] = None
        self._timer = None

    def set_excluded(self, predicate: Optional[Callable[[int], bool]]):
        """注册"这个窗口不能当粘贴目标"的判断（拾取窗口自己）。"""
        self._is_excluded = predicate

    @property
    def target_hwnd(self) -> Optional[int]:
        """最后一次采到的粘贴目标；窗口已销毁时返回 None。"""
        if not is_alive(self._target_hwnd):
            return None
        return self._target_hwnd

    def start(self):
        """立即取样一次，然后开始周期取样。"""
        self.sample()
        if self._timer is None:
            from PySide6.QtCore import QTimer

            self._timer = QTimer()
            self._timer.setInterval(self._interval_ms)
            self._timer.timeout.connect(self.sample)
        self._timer.start()

    def stop(self):
        if self._timer is not None:
            self._timer.stop()

    def set_interval(self, interval_ms: int):
        """更新取样间隔，定时器运行中也立即生效。"""
        self._interval_ms = interval_ms
        if self._timer is not None:
            self._timer.setInterval(interval_ms)

    def sample(self) -> bool:
        """取样一次，返回是否更新了目标。

        由定时器驱动，异常不能逃出去——PySide 下槽函数里未捕获的异常会终止进程。
        排除判断由窗口侧注入，窗口销毁后可能抛 RuntimeError。
        """
        try:
            hwnd = self._read_candidate()
        except Exception as e:
            log_exception(e, T("取样前台窗口"))
            return False

        if hwnd is None or hwnd == self._target_hwnd:
            return False
        self._target_hwnd = hwnd
        log_debug(
            T("粘贴目标更新为 {hwnd} ({cls})", hwnd=hwnd, cls=get_window_class(hwnd)),
            "Clipboard",
        )
        return True

    def _read_candidate(self) -> Optional[int]:
        """读取当前前台窗口，不合格返回 None（调用方保留旧值）。"""
        hwnd = get_foreground_hwnd()
        if not is_alive(hwnd):
            return None
        if not can_take_focus(hwnd):
            return None
        if get_window_class(hwnd) in _SHELL_CLASSES:
            return None
        if self._is_excluded is not None and self._is_excluded(hwnd):
            return None
        return hwnd
