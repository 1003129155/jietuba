# -*- coding: utf-8 -*-
"""
剪切板图像写入工具
Windows 使用 Win32 写入 CF_DIBV5 + PNG；若已知保存路径，同一次事务里
一并写入 CF_HDROP，供只认文件路径、不解析位图格式的粘贴目标（如部分
终端 CLI）使用。截图出口的写入放在后台线程，界面线程不等编码。
"""

from __future__ import annotations

import io
import struct
import sys
import threading
from collections import deque
from typing import TYPE_CHECKING

from PySide6.QtCore import QBuffer, QIODeviceBase, QObject, Qt, Signal, Slot
from PySide6.QtGui import QImage

from core import log_info, log_warning, log_debug, T

if TYPE_CHECKING:
    from core.save import SaveService


_CLIPBOARD_WRITE_LOCK = threading.Lock()
_CLIPBOARD_BUSY_HRESULT = -2147221040  # 0x800401D0 = CLIPBRD_E_CANT_OPEN
_CLIPBOARD_WRITE_RETRY_DELAYS = (0.0, 0.02, 0.05, 0.1, 0.2, 0.35)


def copy_image_to_clipboard(image: QImage, file_reference: str | None = None) -> None:
    """将 QImage 复制到系统剪切板。

    Windows 下使用 Win32 方式写入 `CF_DIBV5 + PNG`，
    并在剪切板被占用时自动重试。

    file_reference 不为空时，会在同一次剪切板事务里一并写入 CF_HDROP。
    合并进同一次事务是为了只触发一次"剪切板内容变化"通知——分两次
    写入会被剪切板历史一类的监听者当成两条独立记录。调用方需确保
    该路径此时已经存在（哪怕是空文件），否则粘贴目标可能读到空内容。
    """
    if image.isNull():
        log_warning(T("剪切板: 图像为空"), "Clipboard")
        return

    if sys.platform == "win32":
        try:
            _copy_win32(image, file_reference)
            return
        except Exception as e:
            log_warning(T("剪切板: Win32 写入失败 ({e})", e=e), "Clipboard")

    # 非 Windows 平台或 Win32 失败时回退
    _copy_qt_fallback(image)


class Delivery:
    """一次投递在后台的进度：剪贴板和保存都做完后 join() 返回 True。"""

    def __init__(self, parts: int):
        self._pending = parts
        self._lock = threading.Lock()
        self._done = threading.Event()

    def _part_done(self) -> None:
        with self._lock:
            self._pending -= 1
            finished = self._pending <= 0
        if finished:
            self._done.set()

    def join(self, timeout: float | None = None) -> bool:
        return self._done.wait(timeout)

    def is_alive(self) -> bool:
        return not self._done.is_set()


def deliver_image_async(
    image: QImage,
    *,
    copy_to_clipboard: bool = True,
    save_service: "SaveService | None" = None,
    save_kwargs: dict | None = None,
    write_file_reference: bool = True,
    original: QImage | None = None,
) -> Delivery | None:
    """在后台复制到剪贴板、按需保存图像，调用方不等编码。

    Windows 下写剪贴板（编码位图和 PNG）排进同一条后台队列依次执行，连续截图时
    剪贴板留下的总是最后一张；代价是剪贴板要晚几十毫秒才是新图。
    同时请求复制与保存时，先用 SaveService.reserve_save_path 占好保存路径
    （只创建空文件），复制时就能把 CF_HDROP 和位图格式一起写入剪贴板，不必等
    图像编码落盘。write_file_reference=False 时跳过这一步，剪贴板上只有位图格式。
    original 不为空时，存好的文件旁边再存一张它（文件名末尾加 _original）。
    """
    if image is None or image.isNull():
        log_warning(T("图像投递: 图像为空，跳过"), "Clipboard")
        return None

    if not copy_to_clipboard and save_service is None:
        log_warning(T("图像投递: 未请求复制或保存，跳过"), "Clipboard")
        return None

    save_kwargs = dict(save_kwargs or {})

    if not copy_to_clipboard or sys.platform != "win32":
        if copy_to_clipboard:
            log_debug(T("图像投递: 非 Windows 平台，剪贴板仍走主线程回退"), "Clipboard")
            copy_image_to_clipboard(image)
        if save_service is None:
            return None
        delivery = Delivery(1)
        _start_save(delivery, image, save_service, save_kwargs, None, original)
        return delivery

    delivery = Delivery(2 if save_service is not None else 1)
    reserve = save_service is not None and write_file_reference
    _main_thread_clipboard()

    def job() -> None:
        try:
            target_path = None
            if reserve:
                try:
                    target_path = save_service.reserve_save_path(**save_kwargs)
                except Exception as exc:
                    # 保存目录不可写（外接盘拔掉、网络盘断连）时占不到路径，降级成不带文件引用
                    log_warning(T("图像投递: 预留保存路径失败，剪贴板不写入文件路径 ({exc})", exc=exc), "Clipboard")
            if save_service is not None:
                _start_save(delivery, image, save_service, save_kwargs, target_path, original)
            copy_image_to_clipboard(image, file_reference=target_path)
        finally:
            delivery._part_done()

    _enqueue_clipboard_job(job)
    return delivery


def _start_save(delivery, image, save_service, save_kwargs, target_path, original) -> None:
    def worker() -> None:
        nonlocal image, original
        import time as _time

        started = _time.perf_counter()
        try:
            save_ok, saved_path = save_service.save_qimage(image, target_path=target_path, **save_kwargs)
            if save_ok and original is not None:
                save_service.save_beside(original, saved_path, "original")
            log_debug(T("图像投递: 后台保存完成 save={save_ok} {ms:.1f}ms",
                        save_ok=save_ok, ms=(_time.perf_counter() - started) * 1000), "Clipboard")
        except Exception as exc:
            log_warning(T("图像投递: 后台任务失败 ({exc})", exc=exc), "Clipboard")
        finally:
            image = None
            original = None
            delivery._part_done()

    from core.background_tasks import start_thread
    try:
        start_thread(worker, name="ClipboardDeliverSave")
    except RuntimeError as exc:  # 准备更新时不再起新线程
        log_warning(T("图像投递: 后台任务失败 ({exc})", exc=exc), "Clipboard")
        delivery._part_done()


# ─── 后台剪贴板队列 ───────────────────────────────────────────────────

_clipboard_jobs: deque = deque()
_clipboard_jobs_lock = threading.Lock()
_clipboard_idle = threading.Event()
_clipboard_idle.set()
_clipboard_worker_running = False


def _enqueue_clipboard_job(job) -> None:
    """排进剪贴板队列。队列空了线程就退出，不常驻，免得更新程序一直等不到空闲。"""
    global _clipboard_worker_running
    with _clipboard_jobs_lock:
        _clipboard_jobs.append(job)
        _clipboard_idle.clear()
        if _clipboard_worker_running:
            return
        _clipboard_worker_running = True

    from core.background_tasks import start_thread
    try:
        start_thread(_run_clipboard_jobs, name="ClipboardWrite")
    except RuntimeError:
        # 准备更新时不再起新线程，就地执行，复制照常生效
        _run_clipboard_jobs()


def _run_clipboard_jobs() -> None:
    global _clipboard_worker_running
    while True:
        with _clipboard_jobs_lock:
            if not _clipboard_jobs:
                _clipboard_worker_running = False
                _clipboard_idle.set()
                return
            job = _clipboard_jobs.popleft()
        try:
            job()
        except Exception as exc:
            log_warning(T("图像投递: 后台任务失败 ({exc})", exc=exc), "Clipboard")


def wait_for_clipboard_writes(timeout: float | None = None) -> bool:
    """等排队的剪贴板写入做完；退出程序前调用，免得最后一张截图没写进剪贴板。"""
    return _clipboard_idle.wait(timeout)


class _MainThreadClipboard(QObject):
    """把后台线程要做的 Qt 剪贴板回退排回主线程执行。"""

    fallback_requested = Signal(object)

    def __init__(self):
        super().__init__()
        self.fallback_requested.connect(self._copy, Qt.ConnectionType.QueuedConnection)

    @Slot(object)
    def _copy(self, image) -> None:
        _copy_qt_fallback(image)


_main_thread_clipboard_instance: _MainThreadClipboard | None = None


def _main_thread_clipboard() -> _MainThreadClipboard:
    """第一次投递时在主线程创建，之后后台线程只发信号。"""
    global _main_thread_clipboard_instance
    if _main_thread_clipboard_instance is None:
        _main_thread_clipboard_instance = _MainThreadClipboard()
    return _main_thread_clipboard_instance


def deliver_screenshot(
    image: QImage,
    config_manager,
    *,
    copy_to_clipboard: bool = True,
    trigger: str = "capture",
    file_prefix: str = "",
    original: QImage | None = None,
    save_service: "SaveService | None" = None,
) -> Delivery | None:
    """所有截图的出口：按需复制到剪贴板，这次截图的时机开着自动保存时在后台存成文件。

    trigger 是自动保存的时机："capture" 截图完成，"pin" 生成钉图（钉图由调用方创建）。
    original 是加了标注或水印之前的图，没加工过就不传；开着「同时保存原图」时另存一份。
    save_service 沿用调用方已有的实例，不传就新建。
    """
    save_kwargs = None
    write_file_reference = True
    if _auto_save_enabled(config_manager, trigger):
        if save_service is None:
            from core.save import SaveService
            save_service = SaveService(config_manager=config_manager)
        save_kwargs = dict(
            directory=config_manager.get_screenshot_save_path(),
            prefix=file_prefix,
            image_format=save_service.format_for_size(
                config_manager.get_screenshot_format(), image.width(), image.height()),
        )
        # 文件路径要等自动保存落盘才有，这个开关只在自动保存开着时读
        write_file_reference = config_manager.get_clipboard_file_reference_enabled()
        if not config_manager.get_screenshot_save_original_enabled():
            original = None
    else:
        save_service = None
        original = None

    if not copy_to_clipboard and save_service is None:
        return None
    return deliver_image_async(
        image,
        copy_to_clipboard=copy_to_clipboard,
        save_service=save_service,
        save_kwargs=save_kwargs,
        write_file_reference=write_file_reference,
        original=original,
    )


def _auto_save_enabled(config_manager, trigger: str) -> bool:
    if config_manager is None:
        return False
    switches = {
        "capture": config_manager.get_screenshot_save_enabled,
        "pin": config_manager.get_screenshot_pin_save_enabled,
    }
    return switches[trigger]()


# ─── Win32 实现 ───────────────────────────────────────────────────────

def _copy_win32(image: QImage, file_reference: str | None) -> None:
    """用 Win32 API 写入 CF_DIBV5 + PNG（可选同时写入 CF_HDROP）。"""
    _run_clipboard_write_with_retry(lambda: _copy_win32_legacy(image, file_reference), "Win32")


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
                T(
                    "剪贴板: {path_name} 写入时剪贴板被占用，准备重试 {attempt_next}/{total_attempts}",
                    path_name=path_name,
                    attempt_next=attempt_index + 1,
                    total_attempts=total_attempts,
                ),
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


def _copy_win32_legacy(image: QImage, file_reference: str | None) -> None:
    """用 Win32 API 直接写入 CF_DIBV5 + PNG（可选 CF_HDROP）到系统剪切板。"""
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
    hdrop_data = _build_hdrop(file_reference) if file_reference else None

    with _CLIPBOARD_WRITE_LOCK:
        win32clipboard.OpenClipboard(0)
        try:
            win32clipboard.EmptyClipboard()
            win32clipboard.SetClipboardData(fmt_png, png_data)     # "PNG"
            win32clipboard.SetClipboardData(17, dibv5_data)       # CF_DIBV5 = 17
            if hdrop_data is not None:
                win32clipboard.SetClipboardData(15, hdrop_data)  # CF_HDROP = 15
        finally:
            win32clipboard.CloseClipboard()
    _t3 = _time.perf_counter()

    log_debug(
        T(
            "已复制到剪切板 (Win32) "
            "dibv5={dibv5_ms:.1f}ms png={png_ms:.1f}ms win32={win32_ms:.1f}ms",
            dibv5_ms=(_t1 - _t0) * 1000,
            png_ms=(_t2 - _t1) * 1000,
            win32_ms=(_t3 - _t2) * 1000,
        ),
        "Clipboard"
    )
    log_info(T("已复制到剪切板 (Win32 CF_DIBV5 + PNG)"), "Clipboard")


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


def _build_hdrop(path: str) -> bytes:
    """构造 CF_HDROP 所需的 DROPFILES 结构（含单个文件路径）。"""
    name = path.encode("utf-16-le") + b"\x00\x00" + b"\x00\x00"  # 双 NUL 结尾
    header = struct.pack("<Iiiii", 20, 0, 0, 0, 1)  # pFiles=20（结构体大小），fWide=1
    return header + name


# ─── Qt 回退实现 ──────────────────────────────────────────────────────

def _copy_qt_fallback(image: QImage) -> None:
    """非 Windows 平台或 Win32 写入失败时的回退：用 Qt setImage。

    QClipboard 只能在主线程用，后台线程调用时排回主线程执行。
    """
    if threading.current_thread() is not threading.main_thread():
        relay = _main_thread_clipboard_instance
        if relay is None:
            log_warning(T("剪切板: 后台线程无法回退到 Qt 写入"), "Clipboard")
            return
        relay.fallback_requested.emit(image)
        return
    from PySide6.QtWidgets import QApplication
    QApplication.clipboard().setImage(image)
    log_info(T("已复制到剪切板 (Qt)"), "Clipboard")
