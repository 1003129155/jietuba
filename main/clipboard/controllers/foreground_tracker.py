# -*- coding: utf-8 -*-
"""前台窗口跟踪：记住最后一个可作为粘贴目标的前台窗口。

粘贴是"把焦点还给目标窗口 + 模拟 Ctrl+V"。keybd_event 没有收件人，按键落到谁
身上取决于那一刻谁持有焦点，所以必须先知道该把焦点还给谁。

窗口失焦即隐藏时，show 时取一次样就够——记录和使用之间用户没有机会切换窗口。
窗口一旦可以常驻（粘贴后不关闭），中途焦点会反复变化，必须持续跟踪：显示期间订阅
输入中心的前台窗口事件，每一次切换都按顺序送来，切过去点一下马上切回来也不会漏。

每次都要排除拾取窗口自己：用户点在它上面时前台就是它，此刻记录等于把粘贴目标改
成自己，Ctrl+V 会落进搜索框。但只排除它一个——本应用的其他窗口（内容编辑、截图
标注、翻译）都是合法的粘贴目标，按进程排除会把它们一起误伤。同样排除任务栏、
桌面，以及拿不到键盘焦点的窗口——它们收不到模拟按键。
不合格时保留上一次的值，不回退到当前前台。
"""

import ctypes
from ctypes import wintypes
from typing import Callable, Optional

from PySide6.QtCore import Qt

from core.input_hub import input_hub
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

_WATCHER = "clipboard"


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
    """保留最后一个可作为粘贴目标的前台窗口。

    前台切换事件经排队回到界面线程再判断，排除条件可以放心读 Qt 窗口。
    """

    def __init__(self):
        self._target_hwnd: Optional[int] = None
        self._is_excluded: Optional[Callable[[int], bool]] = None
        self._hub = None

    def set_excluded(self, predicate: Optional[Callable[[int], bool]]):
        """注册"这个窗口不能当粘贴目标"的判断（拾取窗口自己）。"""
        self._is_excluded = predicate

    @property
    def target_hwnd(self) -> Optional[int]:
        """最后一次记下的粘贴目标；窗口已销毁时返回 None。"""
        if not is_alive(self._target_hwnd):
            return None
        return self._target_hwnd

    def start(self):
        """先看一次当前前台，再跟踪之后的每一次切换。"""
        self.sample()
        if self._hub is None:
            hub = input_hub()
            hub.foreground.connect(self._on_foreground, Qt.ConnectionType.QueuedConnection)
            hub.native.watch_foreground(_WATCHER)
            self._hub = hub

    def stop(self):
        hub, self._hub = self._hub, None
        if hub is not None:
            hub.native.unwatch_foreground(_WATCHER)
            hub.foreground.disconnect(self._on_foreground)

    def sample(self) -> bool:
        """按当前前台窗口更新一次，返回是否更新了目标。"""
        return self._consider(get_foreground_hwnd())

    def _on_foreground(self, hwnd: int):
        # stop 之后才送到的切换不再算
        if self._hub is not None:
            self._consider(hwnd)

    def _consider(self, hwnd: Optional[int]) -> bool:
        """异常不能逃出去——PySide 下槽函数里未捕获的异常会终止进程。
        排除判断由窗口侧注入，窗口销毁后可能抛 RuntimeError。
        """
        try:
            if not self._eligible(hwnd):
                return False
        except Exception as e:
            log_exception(e, T("取样前台窗口"))
            return False

        if hwnd == self._target_hwnd:
            return False
        self._target_hwnd = hwnd
        log_debug(
            T("粘贴目标更新为 {hwnd} ({cls})", hwnd=hwnd, cls=get_window_class(hwnd)),
            "Clipboard",
        )
        return True

    def _eligible(self, hwnd: Optional[int]) -> bool:
        if not is_alive(hwnd):
            return False
        if not can_take_focus(hwnd):
            return False
        if get_window_class(hwnd) in _SHELL_CLASSES:
            return False
        return self._is_excluded is None or not self._is_excluded(hwnd)
