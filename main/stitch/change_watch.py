"""长截图的画面变化监视：后台线程隔一会儿截一次截图区，和上一次不一样就通知窗口去截图。

只判断画面动没动，交给拼接的帧仍由窗口自己截。截屏 DC 的 BitBlt 和逐字节比较都在 GIL 之外，不占界面线程。
画面没动时每 IDLE_MS 看一次；一动就改成每 ACTIVE_MS，连续 ACTIVE_HOLD_MS 没变再放慢。
每次看完才开始等下一次：机器慢时只是看得稀一些，不会积压。
自家工具栏、预览面板压在截图区里时，它们的变化也会触发截图，那一帧拼接时只记位置。
"""

from __future__ import annotations

import ctypes
import threading
import time
from ctypes import wintypes
from typing import Callable, Optional

from PySide6.QtCore import QObject, QRect, Signal

from core.logger import log_exception

IDLE_MS = 300
ACTIVE_MS = 100
ACTIVE_HOLD_MS = 1000

SRCCOPY = 0x00CC0020  # DWM 下分层窗口也截得到，不加 CAPTUREBLT（它会让光标闪）

_user32 = ctypes.WinDLL("user32")
_gdi32 = ctypes.WinDLL("gdi32")
_memcmp = ctypes.CDLL("msvcrt").memcmp
_memcmp.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]
_memcmp.restype = ctypes.c_int
_user32.GetDC.argtypes = [wintypes.HWND]
_user32.GetDC.restype = wintypes.HDC
_user32.ReleaseDC.argtypes = [wintypes.HWND, wintypes.HDC]
_gdi32.CreateCompatibleDC.argtypes = [wintypes.HDC]
_gdi32.CreateCompatibleDC.restype = wintypes.HDC
_gdi32.DeleteDC.argtypes = [wintypes.HDC]
_gdi32.CreateDIBSection.argtypes = [wintypes.HDC, ctypes.c_void_p, wintypes.UINT,
                                    ctypes.POINTER(ctypes.c_void_p), wintypes.HANDLE, wintypes.DWORD]
_gdi32.CreateDIBSection.restype = wintypes.HBITMAP
_gdi32.SelectObject.argtypes = [wintypes.HDC, wintypes.HGDIOBJ]
_gdi32.SelectObject.restype = wintypes.HGDIOBJ
_gdi32.DeleteObject.argtypes = [wintypes.HGDIOBJ]
_gdi32.BitBlt.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                          wintypes.HDC, ctypes.c_int, ctypes.c_int, wintypes.DWORD]
_gdi32.BitBlt.restype = wintypes.BOOL


class _BitmapInfoHeader(ctypes.Structure):
    _fields_ = [("biSize", wintypes.DWORD), ("biWidth", wintypes.LONG), ("biHeight", wintypes.LONG),
                ("biPlanes", wintypes.WORD), ("biBitCount", wintypes.WORD), ("biCompression", wintypes.DWORD),
                ("biSizeImage", wintypes.DWORD), ("biXPelsPerMeter", wintypes.LONG),
                ("biYPelsPerMeter", wintypes.LONG), ("biClrUsed", wintypes.DWORD),
                ("biClrImportant", wintypes.DWORD)]


class ScreenRegion:
    """屏幕上的一块区域（物理像素），交替截进两块缓冲。GDI 句柄归创建它的线程，要在同一线程 close。"""

    def __init__(self, rect: QRect):
        self._x, self._y = rect.x(), rect.y()
        self._w, self._h = rect.width(), rect.height()
        self._size = self._w * self._h * 4
        self._screen = _user32.GetDC(None)
        header = _BitmapInfoHeader(ctypes.sizeof(_BitmapInfoHeader), self._w, -self._h, 1, 32, 0, 0, 0, 0, 0, 0)
        self._buffers = []
        for _ in range(2):
            bits = ctypes.c_void_p()
            bitmap = _gdi32.CreateDIBSection(self._screen, ctypes.byref(header), 0, ctypes.byref(bits), None, 0)
            dc = _gdi32.CreateCompatibleDC(self._screen)
            old = _gdi32.SelectObject(dc, bitmap)
            self._buffers.append((dc, bitmap, old, bits))
        self._current = 0
        self._filled = False

    def changed(self) -> bool:
        """截一次，返回和上一次截的是否不同；第一次、截不到时都当没变。"""
        target = 1 - self._current
        dc, _, _, bits = self._buffers[target]
        if not _gdi32.BitBlt(dc, 0, 0, self._w, self._h, self._screen, self._x, self._y, SRCCOPY):
            return False
        previous = self._buffers[self._current][3]
        self._current = target
        if not self._filled:
            self._filled = True
            return False
        return _memcmp(bits, previous, self._size) != 0

    def close(self) -> None:
        for dc, bitmap, old, _ in self._buffers:
            _gdi32.SelectObject(dc, old)
            _gdi32.DeleteObject(bitmap)
            _gdi32.DeleteDC(dc)
        self._buffers = []
        _user32.ReleaseDC(None, self._screen)


class ChangeWatch(QObject):
    """后台看着截图区，画面一变就发 changed。信号从后台线程发出，接收方要用排队连接。"""

    changed = Signal()

    def __init__(self, rect: QRect, region: Callable[[QRect], ScreenRegion] = ScreenRegion, parent=None):
        super().__init__(parent)
        self._rect = QRect(rect)
        self._region = region
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None

    def start(self) -> None:
        if self._thread is not None:
            return
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, name="stitch-change-watch", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        """停下并等后台线程退出，之后不会再发 changed。"""
        thread, self._thread = self._thread, None
        if thread is None:
            return
        self._stop.set()
        thread.join(2.0)

    def _run(self) -> None:
        try:
            region = self._region(self._rect)
        except Exception as e:
            log_exception(e, "ChangeWatch")
            return
        try:
            last_change = None
            while True:
                active = last_change is not None and time.monotonic() - last_change < ACTIVE_HOLD_MS / 1000
                if self._stop.wait((ACTIVE_MS if active else IDLE_MS) / 1000):
                    break
                if region.changed():
                    last_change = time.monotonic()
                    self.changed.emit()
        except Exception as e:
            log_exception(e, "ChangeWatch")
        finally:
            region.close()
