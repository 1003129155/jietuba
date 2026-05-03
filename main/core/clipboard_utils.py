# -*- coding: utf-8 -*-
"""
剪切板图像写入工具
Windows 优先用 OLE DataObject 提供 PNG + CF_BITMAP，
失败时回退为 Win32 CF_DIBV5 + PNG。
"""

from __future__ import annotations

import io
import struct
import sys
import threading
from array import array
from typing import TYPE_CHECKING

from PySide6.QtCore import QBuffer, QIODeviceBase, QObject, Signal
from PySide6.QtGui import QImage

from core import log_info, log_warning, log_debug

if TYPE_CHECKING:
    from core.save import SaveService


_CLIPBOARD_WRITE_LOCK = threading.Lock()
_FALLBACK_DISPATCHER: "_ClipboardFallbackDispatcher | None" = None
_OLE_DATA_OBJECT_METHODS = """GetData GetDataHere QueryGetData
                              GetCanonicalFormatEtc SetData EnumFormatEtc
                              DAdvise DUnadvise EnumDAdvise""".split()
_OLE_CLIPBOARD_FLAG_ENABLED = struct.pack("<I", 1)
_CLIPBOARD_BUSY_HRESULT = -2147221040  # 0x800401D0 = CLIPBRD_E_CANT_OPEN
_CLIPBOARD_WRITE_RETRY_DELAYS = (0.0, 0.015, 0.03, 0.06)

try:
    import pythoncom as _pythoncom
except Exception:
    _pythoncom = None


class _OleClipboardImageDataObject:
    _com_interfaces_ = [_pythoncom.IID_IDataObject] if _pythoncom is not None else []
    _public_methods_ = _OLE_DATA_OBJECT_METHODS

    def __init__(self, image: QImage) -> None:
        import pythoncom
        import win32clipboard
        import win32con

        self._pythoncom = pythoncom
        self._win32con = win32con
        self._image = image.convertToFormat(QImage.Format.Format_ARGB32)
        self._png_data = _build_png(self._image)
        self._dibv5_data = _build_dibv5(self._image)
        self._fmt_png = win32clipboard.RegisterClipboardFormat("PNG")
        self._fmt_history = win32clipboard.RegisterClipboardFormat("CanIncludeInClipboardHistory")
        self._fmt_cloud = win32clipboard.RegisterClipboardFormat("CanUploadToCloudClipboard")
        self._extra_hglobal_payloads = {
            self._fmt_cloud: array("B", _OLE_CLIPBOARD_FLAG_ENABLED),
            self._fmt_history: array("B", _OLE_CLIPBOARD_FLAG_ENABLED),
        }
        self._supported_formats = [
            (self._fmt_png, None, pythoncom.DVASPECT_CONTENT, -1, pythoncom.TYMED_HGLOBAL),
            (17, None, pythoncom.DVASPECT_CONTENT, -1, pythoncom.TYMED_HGLOBAL),  # CF_DIBV5
            (self._fmt_cloud, None, pythoncom.DVASPECT_CONTENT, -1, pythoncom.TYMED_HGLOBAL),
            (self._fmt_history, None, pythoncom.DVASPECT_CONTENT, -1, pythoncom.TYMED_HGLOBAL),
        ]

    @property
    def supported_formats(self):
        return list(self._supported_formats)

    @property
    def png_format(self) -> int:
        return self._fmt_png

    @property
    def history_format(self) -> int:
        return self._fmt_history

    @property
    def cloud_format(self) -> int:
        return self._fmt_cloud

    def wrap(self):
        from win32com.server.util import wrap

        return wrap(self, iid=self._pythoncom.IID_IDataObject, useDispatcher=0)

    def _query_interface_(self, iid):
        if iid == self._pythoncom.IID_IEnumFORMATETC:
            from win32com.server.util import NewEnum

            return NewEnum(self._supported_formats, iid=iid)
        return None

    def GetData(self, fe):
        import winerror
        from win32com.server.exception import COMException

        cf, _target, aspect, _index, tymed = fe
        if aspect & self._pythoncom.DVASPECT_CONTENT == 0:
            raise COMException(hresult=winerror.DV_E_DVASPECT)

        if cf == 17 and tymed & self._pythoncom.TYMED_HGLOBAL:  # CF_DIBV5
            medium = self._pythoncom.STGMEDIUM()
            medium.set(self._pythoncom.TYMED_HGLOBAL, array("B", self._dibv5_data))
            return medium

        if cf == self._fmt_png and tymed & self._pythoncom.TYMED_HGLOBAL:
            medium = self._pythoncom.STGMEDIUM()
            medium.set(self._pythoncom.TYMED_HGLOBAL, array("B", self._png_data))
            return medium

        payload = self._extra_hglobal_payloads.get(cf)
        if payload is not None and tymed & self._pythoncom.TYMED_HGLOBAL:
            medium = self._pythoncom.STGMEDIUM()
            medium.set(self._pythoncom.TYMED_HGLOBAL, payload)
            return medium

        raise COMException(hresult=winerror.DV_E_FORMATETC)

    def GetDataHere(self, fe):
        import winerror
        from win32com.server.exception import COMException

        raise COMException(hresult=winerror.E_NOTIMPL)

    def QueryGetData(self, fe):
        import winerror
        from win32com.server.exception import COMException

        cf, _target, aspect, _index, tymed = fe
        if aspect & self._pythoncom.DVASPECT_CONTENT == 0:
            raise COMException(hresult=winerror.DV_E_DVASPECT)

        for supported_cf, _target, supported_aspect, _index, supported_tymed in self._supported_formats:
            if cf != supported_cf or aspect != supported_aspect:
                continue
            if tymed & supported_tymed:
                return None

        raise COMException(hresult=winerror.DV_E_FORMATETC)

    def GetCanonicalFormatEtc(self, fe):
        import winerror
        from win32com.server.exception import COMException

        raise COMException(hresult=winerror.DATA_S_SAMEFORMATETC)

    def SetData(self, fe, medium):
        import winerror
        from win32com.server.exception import COMException

        raise COMException(hresult=winerror.E_NOTIMPL)

    def EnumFormatEtc(self, direction):
        import winerror
        from win32com.server.exception import COMException
        from win32com.server.util import NewEnum

        if direction != self._pythoncom.DATADIR_GET:
            raise COMException(hresult=winerror.E_NOTIMPL)
        return NewEnum(self._supported_formats, iid=self._pythoncom.IID_IEnumFORMATETC)

    def DAdvise(self, fe, flags, sink):
        import winerror
        from win32com.server.exception import COMException

        raise COMException(hresult=winerror.E_NOTIMPL)

    def DUnadvise(self, connection):
        import winerror
        from win32com.server.exception import COMException

        raise COMException(hresult=winerror.E_NOTIMPL)

    def EnumDAdvise(self):
        import winerror
        from win32com.server.exception import COMException

        raise COMException(hresult=winerror.E_NOTIMPL)


class _ClipboardFallbackDispatcher(QObject):
    fallback_requested = Signal(object)

    def __init__(self) -> None:
        super().__init__()
        self.fallback_requested.connect(self._handle_fallback_request)

    def request_fallback(self, image: QImage) -> None:
        self.fallback_requested.emit(image)

    def _handle_fallback_request(self, image: object) -> None:
        if isinstance(image, QImage) and not image.isNull():
            _copy_qt_fallback(image)


def _get_fallback_dispatcher() -> _ClipboardFallbackDispatcher:
    global _FALLBACK_DISPATCHER
    if _FALLBACK_DISPATCHER is None:
        _FALLBACK_DISPATCHER = _ClipboardFallbackDispatcher()
    return _FALLBACK_DISPATCHER


def copy_image_to_clipboard(image: QImage) -> None:
    """将 QImage 复制到系统剪切板，保留 alpha 透明通道。

        Windows 下优先使用 OLE DataObject：
            - PNG       : 注册格式 "PNG"，优先给 Excel/浏览器等读取，保留透明通道
            - CF_DIBV5  : 32 位 BGRA，兼容大量桌面应用

        若 OLE 写入失败，则回退为：
            - CF_DIBV5  : 32 位 BGRA，兼容大量桌面应用
            - PNG       : 注册格式 "PNG"

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


def deliver_image_async(
    image: QImage,
    *,
    copy_to_clipboard: bool = True,
    save_service: "SaveService | None" = None,
    save_kwargs: dict | None = None,
) -> threading.Thread | None:
    """在单个后台线程中完成复制到剪贴板和可选保存。"""
    if image is None or image.isNull():
        log_warning("图像投递: 图像为空，跳过", "Clipboard")
        return None

    if not copy_to_clipboard and save_service is None:
        log_warning("图像投递: 未请求复制或保存，跳过", "Clipboard")
        return None

    save_kwargs = dict(save_kwargs or {})
    fallback_dispatcher = None

    if copy_to_clipboard and sys.platform != "win32":
        log_debug("图像投递: 非 Windows 平台，剪贴板仍走主线程回退", "Clipboard")
        copy_image_to_clipboard(image)
        copy_to_clipboard = False
    elif copy_to_clipboard:
        fallback_dispatcher = _get_fallback_dispatcher()

    def worker() -> None:
        nonlocal image
        import time as _time

        t0 = _time.perf_counter()
        clipboard_ok = not copy_to_clipboard
        save_ok = save_service is None

        try:
            if copy_to_clipboard:
                try:
                    _copy_win32(image)
                    clipboard_ok = True
                except Exception as exc:
                    log_warning(f"剪贴板: 后台 Win32 写入失败 ({exc})，回退到 Qt 主线程方式", "Clipboard")
                    if fallback_dispatcher is not None:
                        fallback_dispatcher.request_fallback(image)
                        clipboard_ok = True
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
    """优先用 OLE DataObject 写入，失败时回退为 CF_DIBV5 + PNG。"""
    try:
        _run_clipboard_write_with_retry(lambda: _copy_win32_ole(image), "OLE")
        return
    except Exception as exc:
        log_warning(f"剪贴板: OLE 写入失败 ({exc})，回退到 CF_DIBV5 + PNG", "Clipboard")

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


def _copy_win32_ole(image: QImage) -> None:
    """用 OLE DataObject 提供 CF_DIBV5 + PNG，尽量模拟系统截图工具。"""
    import ctypes
    import pythoncom

    import time as _time
    _t0 = _time.perf_counter()

    data_object = _OleClipboardImageDataObject(image)
    _t1 = _time.perf_counter()
    wrapped = data_object.wrap()
    _t2 = _time.perf_counter()

    with _CLIPBOARD_WRITE_LOCK:
        pythoncom.OleInitialize()
        try:
            pythoncom.OleSetClipboard(wrapped)
            # 立即 flush，避免后台线程结束后丢失延迟渲染对象。
            pythoncom.OleFlushClipboard()
        finally:
            # OleFlushClipboard 已释放剪贴板对 DataObject 的引用，
            # 必须在 COM 公寓拆除前显式释放 COM 对象，
            # 否则函数返回后 GC 回收 wrapped 时 Release() 访问已卸载的 COM → 崩溃
            del wrapped
            del data_object
            # OleInitialize 必须配对 OleUninitialize（pythoncom 未暴露此 API）
            ctypes.windll.ole32.OleUninitialize()
    _t3 = _time.perf_counter()

    log_debug(
        f"已复制到剪切板 (Win32 OLE) "
        f"png={(_t1-_t0)*1000:.1f}ms wrap={(_t2-_t1)*1000:.1f}ms ole={(_t3-_t2)*1000:.1f}ms",
        "Clipboard"
    )
    log_info("已复制到剪切板 (Win32 OLE CF_DIBV5 + PNG)", "Clipboard")


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
    """将 QImage 编码为 PNG 字节流。"""
    buf = QBuffer()
    buf.open(QIODeviceBase.OpenModeFlag.WriteOnly)
    # quality=50 → 兼顾速度与体积
    image.save(buf, "PNG", 50)
    buf.close()
    return bytes(buf.data())


# ─── Qt 回退实现 ──────────────────────────────────────────────────────

def _copy_qt_fallback(image: QImage) -> None:
    """非 Windows 平台的回退方案：用 Qt setImage。"""
    from PySide6.QtWidgets import QApplication
    QApplication.clipboard().setImage(image)
    log_info("已复制到剪切板 (Qt)", "Clipboard")
 