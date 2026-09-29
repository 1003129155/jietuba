"""
截图捕获服务 - 负责获取屏幕截图
"""

import ctypes
import threading
from concurrent.futures import ThreadPoolExecutor

import mss
from PySide6.QtGui import QImage
from PySide6.QtCore import QRectF

from core import log_debug, log_warning
from core.logger import log_exception, T

try:
    import hdrcapture
except ImportError:
    hdrcapture = None

# 每个物理输出的 DXGI 等帧预算。0 只取已排队的帧：没有新 present 说明画面没变，缓存帧就是
# 当前画面；非 0 会让静止桌面上的截图一直等到下一次 present。
_HDR_TIMEOUT_MS = 0

# 新会话要等到一次桌面 present 才有首帧。后台建会话后按这个预算等一次首帧，等不到时首次截图回落 mss。
_PRIME_TIMEOUT_MS = 250


class _HdrSession:
    """进程内共享的 DXGI 捕获会话，建、用、关都在一条专用线程上执行。

    - 会话常驻：新会话要等到一次桌面 present 才有首帧，显示器休眠时等不到。
    - pyo3 把会话钉在创建它的线程上，在别的线程释放会被拒绝并泄漏，泄漏的 duplication 会让
      这块屏再也建不起会话。会话只存在 _capture 上，不落进局部变量，异常回溯带不走它。
    - 建会话失败后记住，不在每次截图时重试；forget_failure() / reset() 清除。
    - 每块屏同时只能有一个 duplication：GIF 录制期间会话借给录制线程（lend），give_back()
      之前 acquire 返回 None，不记为失败。
    """

    _executor = ThreadPoolExecutor(max_workers=1, thread_name_prefix="HdrCapture")
    _worker_id = None
    _capture = None
    _failed = False
    _lent = False

    @classmethod
    def _submit(cls, task):
        return cls._executor.submit(cls._run_on_worker, task)

    @classmethod
    def _run_on_worker(cls, task):
        cls._worker_id = threading.get_ident()
        return task()

    @classmethod
    def _run(cls, task):
        """在会话线程上执行并等结果；已在会话线程上时直接执行，否则会等自己而死锁。"""
        if threading.get_ident() == cls._worker_id:
            return task()
        return cls._submit(task).result()

    @classmethod
    def _ensure(cls, remember_failure=True):
        if hdrcapture is None or cls._failed or cls._lent:
            return False
        if cls._capture is None:
            try:
                cls._capture = hdrcapture.Capture(timeout_ms=_HDR_TIMEOUT_MS)
                log_debug(T("HDR 捕获会话已建立"), "CaptureService")
            except Exception as e:
                if remember_failure:
                    cls._failed = True
                log_exception(e, T("建立 HDR 捕获会话失败"))
                return False
        return True

    @classmethod
    def _ensure_primed(cls):
        """后台建会话用：新建的会话顺带等到首帧，已有的会话不动。"""
        created = cls._capture is None
        if cls._ensure(remember_failure=False) and created:
            for monitor in cls._capture.monitors[1:]:
                try:
                    cls._capture.grab(monitor, timeout_ms=_PRIME_TIMEOUT_MS)
                except Exception as e:
                    log_debug(T("HDR 会话预取首帧未完成: {error}", error=e), "CaptureService")

    @classmethod
    def _close(cls):
        if cls._capture is not None:
            cls._capture.close()
            cls._capture = None

    @classmethod
    def call(cls, name, *args, **kwargs):
        """在会话线程上调用 hdrcapture.Capture 的方法；会话已被借出或关闭时抛 RuntimeError。"""
        def invoke():
            if cls._capture is None:
                raise RuntimeError("HDR capture session unavailable")
            return getattr(cls._capture, name)(*args, **kwargs)
        return cls._run(invoke)

    @classmethod
    def get(cls, name):
        """在会话线程上读 hdrcapture.Capture 的属性。"""
        def read():
            if cls._capture is None:
                raise RuntimeError("HDR capture session unavailable")
            return getattr(cls._capture, name)
        return cls._run(read)

    @classmethod
    def acquire(cls):
        """返回会话代理；不可用或已借出时返回 None。会话通常已在后台建好，这里不必等。"""
        if hdrcapture is None or cls._failed or cls._lent:
            return None
        return _SESSION_PROXY if cls._run(cls._ensure) else None

    @classmethod
    def warm_up(cls, only_if=None):
        """后台建会话，不等结果；only_if 在会话线程上判断，为 False 时关掉会话。

        预热失败不记为失败：开机自启时桌面可能还没就绪，留给首次截图重试。
        """
        def sync():
            if only_if is None or only_if():
                cls._ensure_primed()
            else:
                cls._close()
        return cls._submit(sync)

    @classmethod
    def refresh(cls, only_if=None):
        """显示器配置变化后在后台重建会话，only_if 为 False 时只释放。

        借出期间不动，录制线程的会话自己处理配置变化。配置变化后原本建不起来的会话可能能建了，
        所以清掉失败记录。
        """
        def rebuild():
            if cls._lent:
                return
            cls._close()
            cls._failed = False
            if only_if is None or only_if():
                cls._ensure_primed()
        return cls._submit(rebuild)

    @classmethod
    def release(cls):
        """在后台关掉会话，不记为失败，下次 acquire 会重建。"""
        if cls._capture is not None:
            cls._submit(cls._close)

    @classmethod
    def lend(cls, then=None):
        """标记借出并在后台关掉会话；then 在会话关掉之后、同一线程上执行，返回值即 Future 的结果。"""
        cls._lent = True

        def close_then():
            cls._close()
            return then() if then is not None else None
        return cls._submit(close_then)

    @classmethod
    def give_back(cls, after=None):
        """after 先在会话线程上执行（等借用方释放自己的 duplication），之后收回并在后台重建会话。"""
        def reclaim():
            try:
                if after is not None:
                    after()
            finally:
                cls._lent = False
            cls._ensure_primed()
        return cls._submit(reclaim)

    @classmethod
    def forget_failure(cls):
        """清掉建会话失败的记录，下次 acquire 重试。"""
        cls._failed = False

    @classmethod
    def reset(cls):
        """在后台释放会话并清掉失败记录。"""
        cls._failed = False
        return cls._submit(cls._close)

    @classmethod
    def drain(cls):
        """等已提交的任务全部执行完。"""
        cls._submit(lambda: None).result()

    @classmethod
    def shutdown(cls):
        """退出前在会话线程上关掉会话，等关闭执行完再停线程。

        不能用 cancel_futures：排队中的关闭任务被取消后，会话会在解释器退出时由主线程释放，
        被 pyo3 拒绝而泄漏。
        """
        cls._submit(cls._close).result()
        cls._executor.shutdown(wait=True)


class _SessionProxy:
    """acquire() 返回的会话：接口同 hdrcapture.Capture，调用实际在会话线程上执行。"""

    @property
    def monitors(self):
        return _HdrSession.get("monitors")

    def grab(self, *args, **kwargs):
        return _HdrSession.call("grab", *args, **kwargs)

    def grab_region(self, *args, **kwargs):
        return _HdrSession.call("grab_region", *args, **kwargs)


_SESSION_PROXY = _SessionProxy()


def _grab_hdr_frame(session, monitor, adaptive=False):
    """等 DWM 合成完当前改动再取帧：DXGI 取的是合成好的帧，刚关掉的窗口、刚设的截图排除
    要到下一次合成才生效。

    adaptive 见 hdrcapture.Capture.grab：有 HDR 内容时整屏压暗以保留高光，结果随画面变化；
    长截图要求相邻帧重叠部分逐像素一致，只能用固定映射。
    """
    ctypes.windll.dwmapi.DwmFlush()
    frame = session.grab(monitor, timeout_ms=_HDR_TIMEOUT_MS, adaptive=adaptive)
    _log_tone_map_peaks(frame)
    return frame


def _log_tone_map_peaks(frame):
    for info in frame.monitor_info:
        if info.get("tone_map_peak") is not None:
            log_debug(T("HDR 自适应色调映射: 显示器 {index} 峰值 {peak} 倍 SDR 白",
                        index=info["index"], peak=f"{info['tone_map_peak']:.2f}"), "CaptureService")


def grab_region_hdr(rect, adaptive=False):
    """用 HDR 会话抓虚拟桌面上的一块区域（物理像素坐标），返回 QImage。

    拿不到会话时抛 RuntimeError。adaptive 见 _grab_hdr_frame。
    """
    session = _HdrSession.acquire()
    if session is None:
        raise RuntimeError("HDR capture session unavailable")
    return _grab_region(session, rect, adaptive)


def _grab_region(session, rect, adaptive):
    """只读回这块区域：落在单块屏内时在 GPU 上裁好再读回，跨屏才合成整个虚拟桌面再裁。"""
    # 等 DWM 合成的原因见 _grab_hdr_frame。
    ctypes.windll.dwmapi.DwmFlush()
    frame = session.grab_region(rect.x(), rect.y(), rect.width(), rect.height(),
                                timeout_ms=_HDR_TIMEOUT_MS, adaptive=adaptive)
    _log_tone_map_peaks(frame)
    # copy() 不能省：QImage 不持有 bytes 引用。
    return QImage(frame.bgra, frame.width, frame.height, frame.width * 4,
                  QImage.Format.Format_RGB32).copy()


def hdr_display_active():
    """是否有显示器开着 HDR（Windows 高级颜色）。不建会话、不加载显卡驱动，每次截图前都可以调用。"""
    if hdrcapture is None:
        return False
    try:
        return any(display.hdr_enabled for display in hdrcapture.displays())
    except Exception as e:
        log_warning(T("查询显示器 HDR 状态失败，按未开启处理: {error}", error=e), "CaptureService")
        return False


def uses_hdr_engine(engine=None):
    """按截图引擎设置决定这次要不要用 HDR 引擎。

    auto 只在有显示器开着 HDR 时用：SDR 屏上两种引擎结果逐像素相同，而常驻的 HDR 会话要把
    显卡驱动加载进进程、占用显存。每次截图都重新判断，开关 HDR 立即生效。
    """
    if engine is None:
        from settings.tool_settings import get_tool_settings_manager
        engine = get_tool_settings_manager().get_capture_engine()
    return engine == "hdr" or (engine == "auto" and hdr_display_active())


def warm_up_hdr_session(engine="auto"):
    """在后台提前建好 HDR 会话，首次截图不必等建会话；返回 Future。

    auto 下没有显示器开着 HDR 就不建（HDR 状态也在后台判断）。
    """
    return _HdrSession.warm_up(only_if=None if engine == "hdr" else hdr_display_active)


def refresh_hdr_session(engine):
    """显示器配置变化（插拔、改分辨率、开关 HDR）后调用：后台按新配置重建会话，HDR 已关则释放。

    不提前重建时，下一次截图要等 hdrcapture 自己重建会话。
    """
    if engine == "mss":
        return None
    return _HdrSession.refresh(only_if=None if engine == "hdr" else hdr_display_active)


def lend_hdr_session(then=None):
    """GIF 录制窗口打开时调用：在后台关掉截图会话，腾出 duplication 给录制线程。

    返回 Future，结果是 then() 的返回值；then 在会话关掉之后执行。借出期间截图拿不到
    HDR 会话，按引擎设置回落 mss 或报错。
    """
    return _HdrSession.lend(then)


def return_hdr_session(after=None):
    """GIF 录制窗口关闭时调用：after 负责停掉录制线程，之后在后台重建截图会话。"""
    return _HdrSession.give_back(after)


def shutdown_hdr_session():
    """应用退出前调用。"""
    _HdrSession.shutdown()


def apply_capture_engine(engine):
    """设置里保存截图引擎后调用，不阻塞。

    切到 mss 时释放 HDR 会话；切到 auto / hdr 时清掉建会话失败的记录，按新设置在后台建好
    或释放会话。
    """
    if engine == "mss":
        _HdrSession.reset()
    else:
        _HdrSession.forget_failure()
        warm_up_hdr_session(engine)


class CaptureService:
    """多显示器虚拟桌面截图。

    开着 HDR 的显示器上，GDI BitBlt（mss 所用）会把超出桌面白的内容截成纯白；hdrcapture 用
    DXGI Desktop Duplication 在 GPU 上做色调映射。

    engine 见 settings.tool_settings.CAPTURE_ENGINES：auto 在有显示器开着 HDR 时用 HDR、否则用 mss，
    HDR 拿不到会话或失败时也回落 mss；指定 mss / hdr 时只用那一个，失败直接抛异常。
    """

    def __init__(self, engine=None):
        if engine is None:
            from settings.tool_settings import get_tool_settings_manager
            engine = get_tool_settings_manager().get_capture_engine()
        self.engine = engine

    def capture_region(self, rect, cursor=None):
        """截取虚拟桌面上的一块区域，坐标是物理像素，原点可以为负。"""
        if rect.width() <= 0 or rect.height() <= 0:
            raise ValueError("Capture region must have positive dimensions")
        # 和主截图同样用自适应映射，同一块画面两种方式截出来一致。
        image = self._with_engine(
            lambda session: _grab_region(session, rect, adaptive=True),
            lambda: self._capture_region_with_mss(rect),
        )
        _draw_cursor(image, rect.x(), rect.y(), cursor)
        return image

    def capture_all_screens(self, cursor=None):
        """截取整个虚拟桌面，返回 (QImage, 虚拟桌面矩形 QRectF)；给了 cursor 就把指针画进去。"""
        image, rect = self._with_engine(self._capture_with_hdr, self._capture_with_mss)
        _draw_cursor(image, int(rect.x()), int(rect.y()), cursor)
        return image, rect

    def _with_engine(self, capture_hdr, capture_mss):
        if not uses_hdr_engine(self.engine):
            # auto 下 HDR 刚被关掉时，会话留着只占内存
            _HdrSession.release()
            return capture_mss()

        session = _HdrSession.acquire()
        if self.engine == "hdr":
            if session is None:
                raise RuntimeError("HDR capture session unavailable")
            return capture_hdr(session)

        if session is not None:
            try:
                return capture_hdr(session)
            except Exception as e:
                log_warning(T("HDR 截图失败，回落 mss: {error}", error=e), "CaptureService")
        return capture_mss()

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

    @staticmethod
    def _capture_region_with_mss(rect):
        monitor = dict(left=rect.x(), top=rect.y(), width=rect.width(), height=rect.height())
        with mss.mss() as sct:
            shot = sct.grab(monitor)
            return QImage(shot.bgra, shot.width, shot.height, shot.width * 4,
                          QImage.Format.Format_RGB32).copy()

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


def _draw_cursor(image, origin_x, origin_y, cursor):
    """把 SystemCursor 画进截图；origin 是 image 左上角对应的屏幕坐标。

    DXGI 帧和 BitBlt 都不含指针，两条路径统一在这里补画；画指针出错时保留不带指针的截图。
    """
    if cursor is None:
        return
    try:
        cursor.draw_onto(image, origin_x, origin_y)
    except Exception as e:
        log_exception(e, T("绘制鼠标指针"))
