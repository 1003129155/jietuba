"""
截图捕获服务 - 负责获取屏幕截图
"""

import threading

import mss
from PySide6.QtGui import QImage
from PySide6.QtCore import QRectF

from core import log_debug, log_warning
from core.logger import log_exception, T

try:
    import hdrcapture
except ImportError:
    hdrcapture = None

# 每个物理输出的 DXGI 等待预算。预算内等到一次新的 present 才返回，因此拿到的是
# 「此刻」的桌面；给 0 会返回最近一帧，可能早于调用方刚做的改动（例如隐藏自身窗口）。
_HDR_TIMEOUT_MS = 100


class _HdrSession:
    """进程内共享的 DXGI 捕获会话。

    会话必须常驻：新建会话要先等到一次真实的桌面 present 才有首帧，显示器休眠时
    等不到，每次截图都重建就等于每次都冒这个险。

    pyo3 把会话钉在创建它的线程上（D3D11 immediate context 非线程安全），所以只认
    第一个使用它的线程，其余线程回落 mss 而不是抛错。
    """

    _lock = threading.Lock()
    _capture = None
    _owner_thread = None
    _unavailable = hdrcapture is None

    @classmethod
    def acquire(cls):
        """返回可用的会话；不可用时返回 None，由调用方回落 mss。"""
        if cls._unavailable:
            return None

        with cls._lock:
            if cls._capture is None:
                try:
                    cls._capture = hdrcapture.Capture(timeout_ms=_HDR_TIMEOUT_MS)
                    cls._owner_thread = threading.get_ident()
                    log_debug(T("HDR 捕获会话已建立"), "CaptureService")
                except Exception as e:
                    cls._unavailable = True
                    log_exception(e, T("建立 HDR 捕获会话失败，回落 mss"))
                    return None

            if cls._owner_thread != threading.get_ident():
                return None
            return cls._capture

    @classmethod
    def release(cls):
        with cls._lock:
            cls._capture = None
            cls._owner_thread = None


class CaptureService:
    """
    截图捕获服务
    负责获取多显示器虚拟桌面截图

    优先走 hdrcapture（DXGI Desktop Duplication + GPU tone-map）：开启 HDR 的显示器上
    GDI BitBlt 会把超出桌面白的内容硬截断为纯白，mss 用的正是 BitBlt。拿不到会话或
    捕获失败时回落 mss——有图总比没有强，哪怕那张图过曝。
    """

    def capture_all_screens(self):
        """
        捕获所有屏幕

        Returns:
            tuple: (QImage, QRectF)
            - QImage: 包含所有屏幕的完整截图
            - QRectF: 虚拟桌面的几何信息 (x, y, width, height)
        """
        result = self._capture_with_hdr()
        if result is not None:
            return result
        return self._capture_with_mss()

    def _capture_with_hdr(self):
        capture = _HdrSession.acquire()
        if capture is None:
            return None

        try:
            # 索引 0 是完整虚拟桌面。热插拔或改显示设置后会话会重建，因此每次重新读取，
            # 不缓存 monitor 对象。
            monitor = capture.monitors[0]
            frame = capture.grab(monitor, timeout_ms=_HDR_TIMEOUT_MS)
        except Exception as e:
            log_warning(T("HDR 截图失败，回落 mss: {error}", error=e), "CaptureService")
            return None

        # Format_RGB32：内存布局与 ARGB32 相同（小端 BGRA），但告知 Qt alpha 通道
        # 无意义，渲染时跳过 alpha 混合。copy() 不能省：QImage 不持有 bytes 引用。
        qimage = QImage(
            frame.bgra, frame.width, frame.height, frame.width * 4, QImage.Format.Format_RGB32
        ).copy()

        x, y, width, height = monitor.rect
        return qimage, QRectF(x, y, width, height)

    def _capture_with_mss(self):
        with mss.mss() as sct:
            # monitors[0] 是所有显示器的合并区域 (虚拟桌面)
            all_monitors = sct.monitors[0]
            screenshot = sct.grab(all_monitors)

            qimage = QImage(
                screenshot.bgra,
                screenshot.width,
                screenshot.height,
                screenshot.width * 4,
                QImage.Format.Format_RGB32,
            ).copy()

            rect = QRectF(
                all_monitors['left'],
                all_monitors['top'],
                all_monitors['width'],
                all_monitors['height'],
            )
            return qimage, rect
