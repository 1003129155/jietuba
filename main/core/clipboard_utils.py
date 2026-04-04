# -*- coding: utf-8 -*-
"""
剪切板图像写入工具
用 Win32 API 直接写入 CF_DIBV5（32 位 BGRA，保留 alpha）+ PNG 数据，
"""

from __future__ import annotations

import io
import struct
import sys

from PySide6.QtCore import QBuffer, QIODeviceBase
from PySide6.QtGui import QImage

from core import log_info, log_warning, log_debug


def copy_image_to_clipboard(image: QImage) -> None:
    """将 QImage 复制到系统剪切板，保留 alpha 透明通道。

    同时写入：
      - CF_DIBV5  : 32 位 BGRA，大部分 Windows 应用可读（Word/微信/画图等）
      - PNG       : 注册格式 "PNG"，Chrome/Edge 等浏览器优先读取

    Args:
        image: 要复制的图像（任意 QImage 格式，内部会转换）
    """
    if image.isNull():
        log_warning("剪切板: 图像为空", "Clipboard")
        return

    if sys.platform == "win32":
        try:
            _copy_win32(image)
            return
        except Exception as e:
            log_warning(f"剪切板: Win32 写入失败 ({e})，回退到 Qt 方式", "Clipboard")

    # 非 Windows 或 Win32 失败时回退
    _copy_qt_fallback(image)


# ─── Win32 实现 ───────────────────────────────────────────────────────

def _copy_win32(image: QImage) -> None:
    """用 Win32 API 写入 CF_DIBV5 + PNG 到系统剪切板。"""
    import win32clipboard

    import time as _time
    _t0 = _time.perf_counter()

    # 准备数据
    dibv5_data = _build_dibv5(image)
    _t1 = _time.perf_counter()

    png_data = _build_png(image)
    _t2 = _time.perf_counter()

    # 注册 PNG 格式
    fmt_png = win32clipboard.RegisterClipboardFormat("PNG")

    win32clipboard.OpenClipboard(0)
    try:
        win32clipboard.EmptyClipboard()
        win32clipboard.SetClipboardData(17, dibv5_data)       # CF_DIBV5 = 17
        win32clipboard.SetClipboardData(fmt_png, png_data)     # "PNG"
    finally:
        win32clipboard.CloseClipboard()
    _t3 = _time.perf_counter()

    log_debug(
        f"已复制到剪切板 (Win32) "
        f"dibv5={(_t1-_t0)*1000:.1f}ms png={(_t2-_t1)*1000:.1f}ms win32={(_t3-_t2)*1000:.1f}ms",
        "Clipboard"
    )
    log_info("已复制到剪切板 (Win32 CF_DIBV5 + PNG)", "Clipboard")


def _build_dibv5(image: QImage) -> bytes:
    """将 QImage 转为 BITMAPV5HEADER + 32 位 BGRA 像素数据（bottom-up）。"""
    # 转为非预乘 ARGB32（QImage 内存布局：BGRA 小端）
    img = image.convertToFormat(QImage.Format.Format_ARGB32)

    w = img.width()
    h = img.height()
    stride = w * 4  # 32bpp, 总是 4 字节对齐
    pixel_size = stride * h

    # ── BITMAPV5HEADER（124 字节）──
    header = io.BytesIO()
    header.write(struct.pack('<I', 124))          # bV5Size
    header.write(struct.pack('<i', w))            # bV5Width
    header.write(struct.pack('<i', h))            # bV5Height (正 = bottom-up)
    header.write(struct.pack('<H', 1))            # bV5Planes
    header.write(struct.pack('<H', 32))           # bV5BitCount
    header.write(struct.pack('<I', 3))            # bV5Compression = BI_BITFIELDS
    header.write(struct.pack('<I', pixel_size))   # bV5SizeImage
    header.write(struct.pack('<i', 0))            # bV5XPelsPerMeter
    header.write(struct.pack('<i', 0))            # bV5YPelsPerMeter
    header.write(struct.pack('<I', 0))            # bV5ClrUsed
    header.write(struct.pack('<I', 0))            # bV5ClrImportant
    header.write(struct.pack('<I', 0x00FF0000))   # bV5RedMask
    header.write(struct.pack('<I', 0x0000FF00))   # bV5GreenMask
    header.write(struct.pack('<I', 0x000000FF))   # bV5BlueMask
    header.write(struct.pack('<I', 0xFF000000))   # bV5AlphaMask
    header.write(struct.pack('<I', 0x73524742))   # bV5CSType = LCS_sRGB
    header.write(b'\x00' * 36)                    # bV5Endpoints (CIEXYZTRIPLE)
    header.write(struct.pack('<I', 0))            # bV5GammaRed
    header.write(struct.pack('<I', 0))            # bV5GammaGreen
    header.write(struct.pack('<I', 0))            # bV5GammaBlue
    header.write(struct.pack('<I', 4))            # bV5Intent = LCS_GM_IMAGES
    header.write(struct.pack('<I', 0))            # bV5ProfileData
    header.write(struct.pack('<I', 0))            # bV5ProfileSize
    header.write(struct.pack('<I', 0))            # bV5Reserved

    header_bytes = header.getvalue()
    assert len(header_bytes) == 124

    # ── 像素数据：bottom-up ──
    # 用 Qt C++ 层完成垂直翻转，再一次性取出全部像素，避免 Python 逐行循环
    flipped = img.mirrored(False, True)  # 垂直翻转 → bottom-up
    bits = flipped.bits()
    # PySide6: bits() 返回 memoryview，直接转 bytes，无需 setsize()
    return header_bytes + bytes(bits)


def _build_png(image: QImage) -> bytes:
    """将 QImage 编码为 PNG 字节流（最低压缩级别，优先速度）。"""
    buf = QBuffer()
    buf.open(QIODeviceBase.OpenModeFlag.WriteOnly)
    # quality=100 → zlib 最低压缩，速度最快（剪切板是内存传递，体积不敏感）
    image.save(buf, "PNG", 100)
    buf.close()
    return bytes(buf.data())


# ─── Qt 回退实现 ──────────────────────────────────────────────────────

def _copy_qt_fallback(image: QImage) -> None:
    """非 Windows 平台的回退方案：用 Qt setImage。"""
    from PySide6.QtWidgets import QApplication
    QApplication.clipboard().setImage(image)
    log_info("已复制到剪切板 (Qt)", "Clipboard")
 