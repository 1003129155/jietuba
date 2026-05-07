# -*- coding: utf-8 -*-
"""
剪切板图像写入工具
Windows 使用 Win32 写入 CF_DIBV5 + PNG。
"""

from __future__ import annotations

import io
import struct
import sys
import threading
from typing import TYPE_CHECKING

from PySide6.QtCore import QBuffer, QIODeviceBase
from PySide6.QtGui import QImage

from core import log_info, log_warning, log_debug

if TYPE_CHECKING:
    from core.save import SaveService


_CLIPBOARD_WRITE_LOCK = threading.Lock()
_CLIPBOARD_BUSY_HRESULT = -2147221040  # 0x800401D0 = CLIPBRD_E_CANT_OPEN
_CLIPBOARD_WRITE_RETRY_DELAYS = (0.0, 0.02, 0.05, 0.1, 0.2, 0.35)


def copy_image_to_clipboard(image: QImage) -> None:
    """将 QImage 复制到系统剪切板。

    Windows 下使用 Win32 方式写入 `CF_DIBV5 + PNG`，
    并在剪切板被占用时自动重试。
    """
    if image.isNull():
        log_warning("剪切板: 图像为空", "Clipboard")
        return

    if sys.platform == "win32":
        try:
            _copy_win32(image)
            return
        except Exception as e:
            log_warning(f"剪切板: Win32 写入失败 ({e})", "Clipboard")

    # 非 Windows 平台回退
    _copy_qt_fallback(image)


def deliver_image_async(
    image: QImage,
    *,
    copy_to_clipboard: bool = True,
    save_service: "SaveService | None" = None,
    save_kwargs: dict | None = None,
) -> threading.Thread | None:
    """复制到剪贴板并可选在后台线程中保存图像。"""
    if image is None or image.isNull():
        log_warning("图像投递: 图像为空，跳过", "Clipboard")
        return None

    if not copy_to_clipboard and save_service is None:
        log_warning("图像投递: 未请求复制或保存，跳过", "Clipboard")
        return None

    save_kwargs = dict(save_kwargs or {})

    if copy_to_clipboard:
        if sys.platform == "win32":
            copy_image_to_clipboard(image)
        else:
            log_debug("图像投递: 非 Windows 平台，剪贴板仍走主线程回退", "Clipboard")
            copy_image_to_clipboard(image)
        copy_to_clipboard = False

    if save_service is None:
        return None

    def worker() -> None:
        nonlocal image
        import time as _time

        t0 = _time.perf_counter()
        clipboard_ok = not copy_to_clipboard
        save_ok = save_service is None

        try:
            t1 = _time.perf_counter()

            if save_service is not None:
                save_ok, _ = save_service.save_qimage(image, **save_kwargs)
            t2 = _time.perf_counter()

            log_debug(
                f"异步图像投递完成 clipboard={clipboard_ok} save={save_ok} "
                f"clipboard={((t1 - t0) * 1000):.1f}ms save={((t2 - t1) * 1000):.1f}ms total={((t2 - t0) * 1000):.1f}ms",
                "Clipboard"
            )
        except Exception as exc:
            log_warning(f"图像投递: 后台任务失败 ({exc})", "Clipboard")
        finally:
            image = None

    thread = threading.Thread(target=worker, daemon=True, name="ClipboardDeliver")
    thread.start()
    return thread


# ─── Win32 实现 ───────────────────────────────────────────────────────

def _copy_win32(image: QImage) -> None:
    """用 Win32 API 写入 CF_DIBV5 + PNG。"""
    _run_clipboard_write_with_retry(lambda: _copy_win32_legacy(image), "Win32")


def _run_clipboard_write_with_retry(operation, path_name: str) -> None:
    import time as _time

    last_exc = None
    total_attempts = len(_CLIPBOARD_WRITE_RETRY_DELAYS)

    for attempt_index, delay in enumerate(_CLIPBOARD_WRITE_RETRY_DELAYS, start=1):
        if delay > 0:
            _time.sleep(delay)

        try:
            operation()
            return
        except Exception as exc:
            last_exc = exc
            if not _is_clipboard_busy_error(exc) or attempt_index == total_attempts:
                raise

            log_debug(
                f"剪贴板: {path_name} 写入时剪贴板被占用，准备重试 {attempt_index + 1}/{total_attempts}",
                "Clipboard"
            )

    if last_exc is not None:
        raise last_exc


def _is_clipboard_busy_error(exc: Exception) -> bool:
    hresult = getattr(exc, "hresult", None)
    if hresult == _CLIPBOARD_BUSY_HRESULT:
        return True

    args = getattr(exc, "args", ())
    if args:
        if args[0] == _CLIPBOARD_BUSY_HRESULT:
            return True
        if isinstance(args[0], tuple) and args[0] and args[0][0] == _CLIPBOARD_BUSY_HRESULT:
            return True

    return False


def _copy_win32_legacy(image: QImage) -> None:
    """用 Win32 API 直接写入 CF_DIBV5 + PNG 到系统剪切板。"""
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

    with _CLIPBOARD_WRITE_LOCK:
        win32clipboard.OpenClipboard(0)
        try:
            win32clipboard.EmptyClipboard()
            win32clipboard.SetClipboardData(fmt_png, png_data)     # "PNG"
            win32clipboard.SetClipboardData(17, dibv5_data)       # CF_DIBV5 = 17
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
    """将 QImage 编码为 PNG 字节流。"""
    buf = QBuffer()
    buf.open(QIODeviceBase.OpenModeFlag.WriteOnly)
    # quality=50 → 兼顾速度与体积（比100多~14ms，但体积缩小约95%）
    image.save(buf, "PNG", 50)
    buf.close()
    return bytes(buf.data())


# ─── Qt 回退实现 ──────────────────────────────────────────────────────

def _copy_qt_fallback(image: QImage) -> None:
    """非 Windows 平台的回退方案：用 Qt setImage。"""
    from PySide6.QtWidgets import QApplication
    QApplication.clipboard().setImage(image)
    log_info("已复制到剪切板 (Qt)", "Clipboard")
 