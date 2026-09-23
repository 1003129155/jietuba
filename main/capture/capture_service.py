"""
截图捕获服务 - 负责获取屏幕截图
"""

import ctypes
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

# 每个物理输出的 DXGI 等待预算。0 表示取走已排队的 present 但不等待新的：桌面没有新
# present 就意味着画面没变，此时缓存帧正是当前画面。给非 0 值会在静止桌面上一直等到下一次
# present，实测该机空闲时约 66ms 一次，截图反而比 mss 还慢。
_HDR_TIMEOUT_MS = 0


class _HdrSession:
    """进程内共享的 DXGI 捕获会话。

    会话必须常驻：新建会话要先等到一次真实的桌面 present 才有首帧，显示器休眠时
    等不到，每次截图都重建就等于每次都冒这个险。

    pyo3 把会话钉在创建它的线程上（D3D11 immediate context 非线程安全），所以只认
    第一个使用它的线程，其余线程拿到 None。

    建会话失败后记住失败，不在每次截图时重试；forget_failure() / reset() 清掉这个记录。
    """

    _lock = threading.Lock()
    _capture = None
    _owner_thread = None
    _failed = False

    @classmethod
    def acquire(cls):
        """返回可用的会话；不可用时返回 None。"""
        if hdrcapture is None or cls._failed:
            return None

        with cls._lock:
            if cls._capture is None:
                try:
                    cls._capture = hdrcapture.Capture(timeout_ms=_HDR_TIMEOUT_MS)
                    cls._owner_thread = threading.get_ident()
                    log_debug(T("HDR 捕获会话已建立"), "CaptureService")
                except Exception as e:
                    cls._failed = True
                    log_exception(e, T("建立 HDR 捕获会话失败"))
                    return None

            if cls._owner_thread != threading.get_ident():
                return None
            return cls._capture

    @classmethod
    def forget_failure(cls):
        """清掉建会话失败的记录，下次 acquire 重试。"""
        cls._failed = False

    @classmethod
    def reset(cls):
        """释放会话并清掉失败记录。

        必须在会话所属线程调用：unsendable 对象在别的线程上 close 会直接抛错。
        """
        with cls._lock:
            if cls._capture is not None:
                cls._capture.close()
            cls._capture = None
            cls._owner_thread = None
            cls._failed = False


def _grab_hdr_frame(session, monitor, adaptive=False):
    """等 DWM 合成完当前改动再取帧。

    DXGI 拿到的是 DWM 合成好的帧，刚关掉的窗口、刚设的截图排除要到下一次合成才会体现；
    GDI BitBlt 则是同步的。DwmFlush 阻塞到下一次合成完成，实测 3~9ms。

    adaptive 见 hdrcapture.Capture.grab：画面有 HDR 内容时整屏压暗、保留高光层次，结果随内容
    变化；长截图要求相邻帧的重叠部分逐像素一致，只能用默认的固定映射。
    """
    ctypes.windll.dwmapi.DwmFlush()
    frame = session.grab(monitor, timeout_ms=_HDR_TIMEOUT_MS, adaptive=adaptive)
    for info in frame.monitor_info:
        if info.get("tone_map_peak") is not None:
            log_debug(T("HDR 自适应色调映射: 显示器 {index} 峰值 {peak} 倍 SDR 白",
                        index=info["index"], peak=f"{info['tone_map_peak']:.2f}"), "CaptureService")
    return frame


def grab_region_hdr(rect):
    """用 HDR 会话抓虚拟桌面上的一块区域（物理像素坐标），返回 QImage。

    区域落在单块屏内时只回读那块屏，跨屏才回读整个虚拟桌面：多屏时每帧都回读整个
    桌面，瞬时内存和拷贝量都会按屏数翻倍。须在会话所属线程调用，拿不到会话时抛 RuntimeError。
    """
    session = _HdrSession.acquire()
    if session is None:
        raise RuntimeError("HDR capture session unavailable")

    # 热插拔或改显示设置后会话会重建，monitor 列表每次重新读取。
    monitors = session.monitors
    monitor = next((m for m in monitors[1:] if _contains(m.rect, rect)), monitors[0])
    frame = _grab_hdr_frame(session, monitor)

    left, top = monitor.rect[0], monitor.rect[1]
    # QImage 只是借用 frame.bgra 的内存，copy 出区域后就不再引用它。
    full = QImage(frame.bgra, frame.width, frame.height, frame.width * 4, QImage.Format.Format_RGB32)
    return full.copy(rect.x() - left, rect.y() - top, rect.width(), rect.height())


def _contains(monitor_rect, rect):
    x, y, width, height = monitor_rect
    return (x <= rect.x() and y <= rect.y()
            and rect.x() + rect.width() <= x + width
            and rect.y() + rect.height() <= y + height)


def warm_up_hdr_session():
    """在主线程提前建好 HDR 会话，省掉首次截图时建会话的约 100ms。

    预热失败不记为失败：开机自启时桌面可能还没就绪，首次截图时再试一次。
    """
    if _HdrSession.acquire() is not None:
        return True
    _HdrSession.forget_failure()
    return False


def apply_capture_engine(engine):
    """设置里保存截图引擎后调用，须在主线程。

    切到 mss 时释放 HDR 会话，不再占着 DXGI 资源；切到 auto / hdr 时清掉之前的建会话
    失败记录，下次截图重试。
    """
    if engine == "mss":
        _HdrSession.reset()
    else:
        _HdrSession.forget_failure()


class CaptureService:
    """
    截图捕获服务
    负责获取多显示器虚拟桌面截图

    hdrcapture 走 DXGI Desktop Duplication + GPU tone-map：开启 HDR 的显示器上
    GDI BitBlt 会把超出桌面白的内容硬截断为纯白，mss 用的正是 BitBlt。

    engine 取值见 settings.tool_settings.CAPTURE_ENGINES。auto 先走 HDR，拿不到会话或
    捕获失败时回落 mss——有图总比没有强，哪怕那张图过曝；指定 mss / hdr 时失败直接
    抛异常，由调用方记日志。
    """

    def __init__(self, engine=None):
        if engine is None:
            from settings.tool_settings import get_tool_settings_manager
            engine = get_tool_settings_manager().get_capture_engine()
        self.engine = engine

    def capture_all_screens(self):
        """
        捕获所有屏幕

        Returns:
            tuple: (QImage, QRectF)
            - QImage: 包含所有屏幕的完整截图
            - QRectF: 虚拟桌面的几何信息 (x, y, width, height)
        """
        if self.engine == "mss":
            return self._capture_with_mss()

        session = _HdrSession.acquire()
        if self.engine == "hdr":
            if session is None:
                raise RuntimeError("HDR capture session unavailable")
            return self._capture_with_hdr(session)

        if session is not None:
            try:
                return self._capture_with_hdr(session)
            except Exception as e:
                log_warning(T("HDR 截图失败，回落 mss: {error}", error=e), "CaptureService")
        return self._capture_with_mss()

    @staticmethod
    def _capture_with_hdr(session):
        # 索引 0 是完整虚拟桌面。热插拔或改显示设置后会话会重建，因此每次重新读取，
        # 不缓存 monitor 对象。
        monitor = session.monitors[0]
        frame = _grab_hdr_frame(session, monitor, adaptive=True)

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
