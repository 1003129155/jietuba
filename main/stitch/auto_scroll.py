"""长截图的自动滚动：光标停到截图区域中间，一格一格注入滚轮并截图，到底或鼠标一动就停。

两次注入至少隔 STEP_INTERVAL_MS，约每秒四格；每步都等上一帧拼完再滚，截图前窗口会等画面停住。
拼接结论里有这一帧实际挪了多少，连着三格都没挪就是到底了。
"""

from __future__ import annotations

import ctypes
import time
from ctypes import wintypes
from typing import Callable, Optional

from PySide6.QtCore import QObject, QPoint, QTimer, Signal
from PySide6.QtGui import QCursor

WHEEL_DELTA = 120
MOUSEEVENTF_WHEEL = 0x0800
MOUSEEVENTF_HWHEEL = 0x1000

STEP_INTERVAL_MS = 240
SETTLE_MS = 80  # 注入后先等这么久，让页面开始滚，再去等画面停住
POLL_MS = 30  # 查看光标有没有被挪动的间隔
IDLE_STEPS_AT_END = 3  # 页面偶尔晚一步才开始滚，只看一步会误判为到底

SPI_GETMOUSEWHEELROUTING = 0x201C
MOUSEWHEEL_ROUTING_MOUSE_HOVER = 2
GA_ROOT = 2

_user32 = ctypes.WinDLL("user32")
_user32.WindowFromPoint.argtypes = [wintypes.POINT]
_user32.WindowFromPoint.restype = wintypes.HWND
_user32.GetAncestor.argtypes = [wintypes.HWND, wintypes.UINT]
_user32.GetAncestor.restype = wintypes.HWND


def wheel_follows_focus() -> bool:
    """用户关掉了“悬停时滚动非活动窗口”、滚轮发给有焦点的窗口时为 True。"""
    routing = ctypes.c_uint(MOUSEWHEEL_ROUTING_MOUSE_HOVER)
    ok = _user32.SystemParametersInfoW(SPI_GETMOUSEWHEELROUTING, 0, ctypes.byref(routing), 0)
    return bool(ok) and routing.value != MOUSEWHEEL_ROUTING_MOUSE_HOVER


def activate_window_at(x: int, y: int) -> None:
    """把这一点下面的顶层窗口设为前台。截图框鼠标穿透，取到的是被截的那个窗口。"""
    hwnd = _user32.WindowFromPoint(wintypes.POINT(x, y))
    if hwnd:
        _user32.SetForegroundWindow(_user32.GetAncestor(hwnd, GA_ROOT) or hwnd)


def inject_wheel(delta: int, horizontal: bool) -> None:
    """注入一次滚轮，竖向正值往上、横向正值往右，与 WM_MOUSEWHEEL / WM_MOUSEHWHEEL 一致。"""
    flag = MOUSEEVENTF_HWHEEL if horizontal else MOUSEEVENTF_WHEEL
    ctypes.windll.user32.mouse_event(flag, 0, 0, ctypes.c_uint32(delta).value, 0)


class AutoScroller(QObject):
    """自动滚动的节奏。截图、拼接都在窗口那边，这里只决定什么时候滚、什么时候停。

    capture(done) 截一帧并提交拼接，提交后以帧序号调用 done，截不到时为 None；
    窗口收到的每个拼接结论都要交给 on_frame。
    """

    stopped = Signal(str)  # "end" 到底，"mouse" 鼠标动了，"failed" 截不到或接不上，"user" 主动停止

    def __init__(self, capture: Callable[[Callable[[Optional[int]], None]], None],
                 inject: Callable[[int, bool], None] = inject_wheel, cursor=QCursor, parent=None):
        super().__init__(parent)
        self._capture = capture
        self._inject = inject
        self._cursor = cursor
        self._running = False
        self._horizontal = False
        self._sign = -1
        self._idle = 0
        self._injected_at = 0.0
        self._waiting: Optional[int] = None
        self._last_top: Optional[int] = None
        self._parked = QPoint()
        self._settle_timer = QTimer(self)
        self._settle_timer.setSingleShot(True)
        self._settle_timer.timeout.connect(self._capture_frame)
        self._step_timer = QTimer(self)
        self._step_timer.setSingleShot(True)
        self._step_timer.timeout.connect(self._scroll)
        self._poll_timer = QTimer(self)
        self._poll_timer.setInterval(POLL_MS)
        self._poll_timer.timeout.connect(self._check_cursor)

    @property
    def running(self) -> bool:
        return self._running

    def start(self, center: QPoint, horizontal: bool, forward: bool) -> None:
        """把光标停到 center 开始滚：forward 为往下或往右，否则往上或往左。"""
        if self._running:
            return
        self._running = True
        self._horizontal = horizontal
        # 滚轮的正负：竖向正值往上，横向正值往右
        self._sign = (1 if forward else -1) * (1 if horizontal else -1)
        self._idle = 0
        self._waiting = None
        self._cursor.setPos(center)
        self._parked = self._cursor.pos()
        self._poll_timer.start()
        self._scroll()

    def stop(self, reason: str = "user") -> None:
        """停止滚动。已经滚出去、还没截的那一步照常截下，长图才和屏幕上停住的位置一致。"""
        if not self._running:
            return
        self._running = False
        self._poll_timer.stop()
        self._step_timer.stop()
        self._waiting = None
        self.stopped.emit(reason)

    def flush(self) -> None:
        """还没截的那一步立刻截下，马上要导出长图时用。"""
        if self._settle_timer.isActive():
            self._settle_timer.stop()
            self._capture_frame()

    def cancel(self) -> None:
        """停止滚动，还没截的那一步也不截了。"""
        self._settle_timer.stop()
        self.stop()

    def on_frame(self, result) -> None:
        """一帧的拼接结论。不是自动滚动截的帧也要交进来，用来跟踪最新一帧的位置。"""
        moved = None
        if result.ok:
            if self._last_top is not None:
                moved = abs(result.top - result.shift - self._last_top)
            self._last_top = result.top
        if not self._running or result.index != self._waiting:
            return
        self._waiting = None
        if not result.ok:
            self.stop("failed")
            return
        if moved:
            self._idle = 0
        else:
            self._idle += 1
            if self._idle >= IDLE_STEPS_AT_END:
                self.stop("end")
                return
        wait = STEP_INTERVAL_MS - (time.monotonic() - self._injected_at) * 1000
        if wait > 0:
            self._step_timer.start(int(wait))
        else:
            self._scroll()

    def _scroll(self) -> None:
        self._inject(WHEEL_DELTA * self._sign, self._horizontal)
        self._injected_at = time.monotonic()
        self._settle_timer.start(SETTLE_MS)

    def _capture_frame(self) -> None:
        self._capture(self._on_captured)

    def _on_captured(self, index: Optional[int]) -> None:
        if not self._running:
            return
        if index is None:
            self.stop("failed")
            return
        self._waiting = index

    def _check_cursor(self) -> None:
        if (self._cursor.pos() - self._parked).manhattanLength() > 2:
            self.stop("mouse")
