# -*- coding: utf-8 -*-
"""把焦点还给目标窗口，然后补一次 Ctrl+V。

keybd_event 注入的是全局按键事件，没有收件人——按键落到谁身上完全取决于按下
那一刻谁持有焦点。所以顺序只能是：先把目标窗口切到前台，确认切成功，再发按键。

焦点切换是异步的，SetForegroundWindow 返回不代表已经切过去，所以这里用定时
重试代替固定延迟：固定延迟在慢机器上不够、在快机器上白等。
"""

import ctypes
from ctypes import wintypes
from typing import Optional

from PySide6.QtCore import QTimer

from core.logger import T, log_debug, log_exception

from .foreground_tracker import get_current_pid, get_foreground_hwnd, get_window_pid

_user32 = ctypes.windll.user32
_kernel32 = ctypes.windll.kernel32

_user32.SetForegroundWindow.argtypes = [wintypes.HWND]
_user32.SetForegroundWindow.restype = wintypes.BOOL
_user32.BringWindowToTop.argtypes = [wintypes.HWND]
_user32.BringWindowToTop.restype = wintypes.BOOL
_user32.AttachThreadInput.argtypes = [wintypes.DWORD, wintypes.DWORD, wintypes.BOOL]
_user32.AttachThreadInput.restype = wintypes.BOOL
_user32.IsIconic.argtypes = [wintypes.HWND]
_user32.IsIconic.restype = wintypes.BOOL
_user32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
_user32.ShowWindow.restype = wintypes.BOOL
_user32.GetAsyncKeyState.argtypes = [ctypes.c_int]
_user32.GetAsyncKeyState.restype = ctypes.c_short

SW_RESTORE = 9

VK_SHIFT = 0x10
VK_CONTROL = 0x11
VK_MENU = 0x12
VK_LWIN = 0x5B
VK_RWIN = 0x5C
VK_V = 0x56
KEYEVENTF_KEYUP = 0x0002

_MODIFIER_KEYS = (VK_CONTROL, VK_SHIFT, VK_MENU, VK_LWIN, VK_RWIN)
_KEY_DOWN_MASK = 0x8000

# 重试上限约 200ms。超时多半是目标窗口主动拒绝被激活（例如提权窗口），
# 再等下去只是拖慢粘贴，不如按当前焦点发出去。
_ACTIVATE_RETRY_MS = 20
_ACTIVATE_MAX_ATTEMPTS = 10


def set_foreground_window(hwnd: Optional[int]) -> bool:
    """把目标窗口切到前台。

    裸调 SetForegroundWindow 只在本进程持有最后一次输入事件时才被系统放行。
    附加到目标窗口的输入队列可以绕过这条限制，附加期间两个线程共享焦点状态，
    所以用完必须立刻解除。
    """
    if not hwnd:
        return False

    current_thread = None
    target_thread = None
    attached = False
    try:
        # 最小化的窗口不会被 SetForegroundWindow 还原，按键会落进空处
        if _user32.IsIconic(hwnd):
            _user32.ShowWindow(hwnd, SW_RESTORE)
        current_thread = _kernel32.GetCurrentThreadId()
        target_thread = _user32.GetWindowThreadProcessId(hwnd, None)
        if target_thread and target_thread != current_thread:
            attached = bool(_user32.AttachThreadInput(current_thread, target_thread, True))
        _user32.BringWindowToTop(hwnd)
        return bool(_user32.SetForegroundWindow(hwnd))
    except Exception as e:
        log_exception(e, T("设置前台窗口"))
        return False
    finally:
        if attached:
            _user32.AttachThreadInput(current_thread, target_thread, False)


def release_modifiers():
    """松开用户仍然按着的修饰键。

    唤出窗口的热键带 Ctrl，用户松手比我们发按键慢时 Ctrl 还是按下状态，
    此刻发 Ctrl+V，目标窗口收到的是按错的组合键。
    """
    try:
        for vk in _MODIFIER_KEYS:
            if _user32.GetAsyncKeyState(vk) & _KEY_DOWN_MASK:
                _user32.keybd_event(vk, 0, KEYEVENTF_KEYUP, 0)
    except Exception as e:
        log_exception(e, T("释放修饰键"))


def send_ctrl_v() -> bool:
    """模拟按下 Ctrl+V。"""
    try:
        _user32.keybd_event(VK_CONTROL, 0, 0, 0)
        _user32.keybd_event(VK_V, 0, 0, 0)
        _user32.keybd_event(VK_V, 0, KEYEVENTF_KEYUP, 0)
        _user32.keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0)
        return True
    except Exception as e:
        log_exception(e, T("发送 Ctrl+V 失败: {e}", e=e))
        return False


def paste_to_target(hwnd: Optional[int]):
    """切到目标窗口后发送 Ctrl+V。

    hwnd 为空说明目标窗口已经关掉（句柄失效）。这时只能发给当前前台窗口，
    但前台停在自己人身上时宁可不发——按键会被本应用接住。
    """
    if not hwnd:
        if not _foreground_is_own_process():
            _send_paste()
        return
    _activate_then_paste(hwnd, 0)


def _activate_then_paste(hwnd: int, attempt: int):
    if get_foreground_hwnd() == hwnd:
        _send_paste()
        return

    if attempt >= _ACTIVATE_MAX_ATTEMPTS:
        # 前台还停在自己人身上时不能盲发：模拟按键会被本应用的快捷键处理器接住
        if _foreground_is_own_process():
            log_debug(T("目标窗口 {hwnd} 未切到前台，放弃本次补按键", hwnd=hwnd), "Clipboard")
            return
        log_debug(T("目标窗口 {hwnd} 未切到前台，按当前焦点粘贴", hwnd=hwnd), "Clipboard")
        _send_paste()
        return

    set_foreground_window(hwnd)
    QTimer.singleShot(_ACTIVATE_RETRY_MS, lambda: _activate_then_paste(hwnd, attempt + 1))


def _foreground_is_own_process() -> bool:
    hwnd = get_foreground_hwnd()
    if not hwnd:
        return False
    return get_window_pid(hwnd) == get_current_pid()


def _send_paste():
    release_modifiers()
    send_ctrl_v()
