# -*- coding: utf-8 -*-
"""
系统鼠标指针快照 —— 截屏时记下当前指针，事后画进截图

mss（BitBlt）读不到指针，它由系统单独叠加在画面上，只能按指针句柄补画。
补画用 DrawIconEx 直接画在截图像素上：单色指针（如文本 I 形光标）靠反色
显示，先转成带透明度的图片再贴会丢掉反色，在深色背景上就看不见了。
"""

import ctypes
import weakref
from ctypes import wintypes

from PySide6.QtGui import QImage, QPainter

# 独立的 DLL 实例：这里声明的函数签名不影响其他模块共用的 windll.user32
_user32 = ctypes.WinDLL("user32", use_last_error=True)
_gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)

_CURSOR_SHOWING = 0x00000001
_DI_NORMAL = 0x0003


class _CURSORINFO(ctypes.Structure):
    _fields_ = [
        ("cbSize", wintypes.DWORD),
        ("flags", wintypes.DWORD),
        ("hCursor", wintypes.HANDLE),
        ("ptScreenPos", wintypes.POINT),
    ]


class _ICONINFO(ctypes.Structure):
    _fields_ = [
        ("fIcon", wintypes.BOOL),
        ("xHotspot", wintypes.DWORD),
        ("yHotspot", wintypes.DWORD),
        ("hbmMask", wintypes.HBITMAP),
        ("hbmColor", wintypes.HBITMAP),
    ]


class _BITMAP(ctypes.Structure):
    _fields_ = [
        ("bmType", wintypes.LONG),
        ("bmWidth", wintypes.LONG),
        ("bmHeight", wintypes.LONG),
        ("bmWidthBytes", wintypes.LONG),
        ("bmPlanes", wintypes.WORD),
        ("bmBitsPixel", wintypes.WORD),
        ("bmBits", ctypes.c_void_p),
    ]


class _BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ("biSize", wintypes.DWORD),
        ("biWidth", wintypes.LONG),
        ("biHeight", wintypes.LONG),
        ("biPlanes", wintypes.WORD),
        ("biBitCount", wintypes.WORD),
        ("biCompression", wintypes.DWORD),
        ("biSizeImage", wintypes.DWORD),
        ("biXPelsPerMeter", wintypes.LONG),
        ("biYPelsPerMeter", wintypes.LONG),
        ("biClrUsed", wintypes.DWORD),
        ("biClrImportant", wintypes.DWORD),
    ]


_user32.GetCursorInfo.argtypes = [ctypes.POINTER(_CURSORINFO)]
_user32.GetCursorInfo.restype = wintypes.BOOL
_user32.CopyIcon.argtypes = [wintypes.HANDLE]
_user32.CopyIcon.restype = wintypes.HANDLE
_user32.DestroyIcon.argtypes = [wintypes.HANDLE]
_user32.DestroyIcon.restype = wintypes.BOOL
_user32.GetIconInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(_ICONINFO)]
_user32.GetIconInfo.restype = wintypes.BOOL
_user32.DrawIconEx.argtypes = [
    wintypes.HDC, ctypes.c_int, ctypes.c_int, wintypes.HANDLE,
    ctypes.c_int, ctypes.c_int, wintypes.UINT, wintypes.HBRUSH, wintypes.UINT,
]
_user32.DrawIconEx.restype = wintypes.BOOL
_gdi32.GetObjectW.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p]
_gdi32.GetObjectW.restype = ctypes.c_int
_gdi32.DeleteObject.argtypes = [wintypes.HANDLE]
_gdi32.DeleteObject.restype = wintypes.BOOL
_gdi32.CreateCompatibleDC.argtypes = [wintypes.HDC]
_gdi32.CreateCompatibleDC.restype = wintypes.HDC
_gdi32.DeleteDC.argtypes = [wintypes.HDC]
_gdi32.DeleteDC.restype = wintypes.BOOL
_gdi32.CreateDIBSection.argtypes = [
    wintypes.HDC, ctypes.c_void_p, wintypes.UINT,
    ctypes.POINTER(ctypes.c_void_p), wintypes.HANDLE, wintypes.DWORD,
]
_gdi32.CreateDIBSection.restype = wintypes.HBITMAP
_gdi32.SelectObject.argtypes = [wintypes.HDC, wintypes.HANDLE]
_gdi32.SelectObject.restype = wintypes.HANDLE
_gdi32.GdiFlush.argtypes = []
_gdi32.GdiFlush.restype = wintypes.BOOL


def _icon_geometry(handle):
    """返回指针图像的 (宽, 高, 热点x, 热点y)，取不到时返回 None。"""
    info = _ICONINFO()
    if not _user32.GetIconInfo(handle, ctypes.byref(info)):
        return None
    try:
        bitmap = _BITMAP()
        source = info.hbmColor or info.hbmMask
        if not _gdi32.GetObjectW(source, ctypes.sizeof(_BITMAP), ctypes.byref(bitmap)):
            return None
        height = bitmap.bmHeight
        if not info.hbmColor:
            # 单色指针的掩码位图上下叠放 AND、XOR 两半
            height //= 2
        return bitmap.bmWidth, height, info.xHotspot, info.yHotspot
    finally:
        _gdi32.DeleteObject(info.hbmMask)
        if info.hbmColor:
            _gdi32.DeleteObject(info.hbmColor)


class SystemCursor:
    """截屏那一刻的系统指针，持有一份指针句柄副本，对象回收时释放。"""

    def __init__(self, handle, x, y, width, height):
        # x, y 是指针图像左上角的屏幕物理坐标，已扣除热点
        self.x = x
        self.y = y
        self.width = width
        self.height = height
        self._handle = handle
        weakref.finalize(self, _user32.DestroyIcon, handle)

    @classmethod
    def grab(cls):
        """记下当前指针；指针处于隐藏状态或取不到时返回 None。"""
        info = _CURSORINFO(cbSize=ctypes.sizeof(_CURSORINFO))
        if not _user32.GetCursorInfo(ctypes.byref(info)):
            return None
        if not info.flags & _CURSOR_SHOWING or not info.hCursor:
            return None
        # 原句柄归前台程序所有，可能在会话中途被它销毁；刷新背景时还要再画
        handle = _user32.CopyIcon(info.hCursor)
        if not handle:
            return None
        geometry = _icon_geometry(handle)
        if geometry is None:
            _user32.DestroyIcon(handle)
            return None
        width, height, hotspot_x, hotspot_y = geometry
        return cls(handle,
                   info.ptScreenPos.x - hotspot_x, info.ptScreenPos.y - hotspot_y,
                   width, height)

    def draw_onto(self, image: QImage, origin_x: int, origin_y: int):
        """把指针画进 image；origin 是 image 左上角对应的屏幕坐标。"""
        left = self.x - origin_x
        top = self.y - origin_y
        x0, y0 = max(left, 0), max(top, 0)
        x1 = min(left + self.width, image.width())
        y1 = min(top + self.height, image.height())
        if x0 >= x1 or y0 >= y1:
            return
        width, height = x1 - x0, y1 - y0
        stride = width * 4

        header = _BITMAPINFOHEADER(
            biSize=ctypes.sizeof(_BITMAPINFOHEADER),
            biWidth=width, biHeight=-height,   # 负高度 = 自上而下，与 QImage 行序一致
            biPlanes=1, biBitCount=32,
        )
        bits = ctypes.c_void_p()
        hdc = _gdi32.CreateCompatibleDC(None)
        if not hdc:
            return
        dib = _gdi32.CreateDIBSection(hdc, ctypes.byref(header), 0, ctypes.byref(bits), None, 0)
        if not dib:
            _gdi32.DeleteDC(hdc)
            return
        previous = _gdi32.SelectObject(hdc, dib)
        try:
            patch = image.copy(x0, y0, width, height).convertToFormat(QImage.Format.Format_RGB32)
            ctypes.memmove(bits, bytes(patch.constBits()), stride * height)
            if not _user32.DrawIconEx(hdc, left - x0, top - y0, self._handle,
                                      self.width, self.height, 0, None, _DI_NORMAL):
                return
            _gdi32.GdiFlush()
            pixels = bytearray(ctypes.string_at(bits, stride * height))
        finally:
            _gdi32.SelectObject(hdc, previous)
            _gdi32.DeleteObject(dib)
            _gdi32.DeleteDC(hdc)

        # 单色指针的掩码运算会把整块 alpha 字节清零，而 RGB32 要求它恒为 0xFF
        pixels[3::4] = b"\xff" * (width * height)
        drawn = QImage(pixels, width, height, stride, QImage.Format.Format_RGB32)
        painter = QPainter(image)
        painter.setCompositionMode(QPainter.CompositionMode.CompositionMode_Source)
        painter.drawImage(x0, y0, drawn)
        painter.end()
