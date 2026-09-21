"""UIA snapshots rooted at pre-capture HWNDs so the overlay cannot intercept them."""

from __future__ import annotations

import atexit
import ctypes
from dataclasses import dataclass, field
import importlib.util
import itertools
from collections import deque
from queue import Empty, SimpleQueue
import sys
import threading
import time
from typing import Iterable

from PySide6.QtCore import QObject, QTimer, Signal

from core import log_debug
from core.logger import T


Rect = tuple[int, int, int, int]


@dataclass(frozen=True)
class UIAElementSnapshot:
    rect: Rect
    order: int = 0

    @property
    def area(self) -> int:
        left, top, right, bottom = self.rect
        return (right - left) * (bottom - top)

    def contains(self, x: int, y: int) -> bool:
        left, top, right, bottom = self.rect
        return left <= x < right and top <= y < bottom


def is_uia_available() -> bool:
    return sys.platform == "win32" and importlib.util.find_spec("comtypes") is not None


def is_hung_window(hwnd: int) -> bool:
    """窗口是否已经不处理消息了。它的 UIA 提供方多半也不会回话。"""
    try:
        return bool(ctypes.windll.user32.IsHungAppWindow(hwnd))
    except Exception:
        return False


class ElementIndex:
    """一个窗口的控件矩形，按面积预排序并按 y 分桶。

    命中测试落在每一个鼠标移动事件上，而一个 Chrome 窗口能报出四千多个矩形。
    逐个扫一遍要 0.45 ms，125 Hz 的鼠标下常驻吃掉 6% 的单核；只扫鼠标所在那
    一横条之后是 0.02 ms。排序也提到构建时做一次，查询不再 sort。
    """

    # 桶高按控件尺度取：再高就退化成全表扫描，再矮则大矩形要写进太多个桶。
    BUCKET_PX = 64

    def __init__(self, elements: Iterable[UIAElementSnapshot] = ()):
        # 面积相同时取树里靠后的那个：同一块矩形被父子重复上报时，子元素才是
        # 鼠标真正指着的东西。
        ordered = sorted(elements, key=lambda element: (element.area, -element.order))
        self._count = len(ordered)
        buckets: dict[int, list] = {}
        for element in ordered:
            left, top, right, bottom = element.rect
            row = (left, top, right, bottom, element)
            for bucket in range(top // self.BUCKET_PX, (bottom - 1) // self.BUCKET_PX + 1):
                buckets.setdefault(bucket, []).append(row)
        self._buckets = {key: tuple(rows) for key, rows in buckets.items()}

    def __len__(self) -> int:
        return self._count

    def at(self, x: int, y: int, *, min_size=(1, 1)) -> tuple[UIAElementSnapshot, ...]:
        """命中该点的元素，由细到粗排好——滚轮切粒度就是沿这条链往外走。"""
        min_width, min_height = min_size
        return tuple(
            row[4] for row in self._buckets.get(y // self.BUCKET_PX, ())
            if row[0] <= x < row[2] and row[1] <= y < row[3]
            and row[2] - row[0] >= min_width and row[3] - row[1] >= min_height
        )


def _rect_from_uia(rect) -> Rect | None:
    try:
        result = (int(rect.left), int(rect.top), int(rect.right), int(rect.bottom))
    except (AttributeError, TypeError, ValueError, OverflowError):
        return None
    return result if result[2] > result[0] and result[3] > result[1] else None


class _UIABackend:
    """Created, used and released exclusively on the worker's COM MTA."""

    MAX_ELEMENTS = 4096

    def __init__(self):
        self._automation = self._client = self._cache_request = None
        self._condition = None
        self._comtypes = None
        first_import = "comtypes" not in sys.modules
        previous_flags = getattr(sys, "coinit_flags", None)
        try:
            # The first import initializes its own thread. Subsequent imports
            # do not, so explicitly initialize that case as well.
            sys.coinit_flags = 0
            import comtypes
        finally:
            if previous_flags is None:
                del sys.coinit_flags
            else:
                sys.coinit_flags = previous_flags
        if not first_import:
            comtypes.CoInitializeEx(comtypes.COINIT_MULTITHREADED)
        self._comtypes = comtypes
        automation = cache = client = None
        try:
            import comtypes.client

            comtypes.client.gen_dir = None
            client = comtypes.client.GetModule("UIAutomationCore.dll")
            try:
                automation = comtypes.client.CreateObject(
                    client.CUIAutomation8, interface=client.IUIAutomation2
                )
                automation.ConnectionTimeout = 300
                automation.TransactionTimeout = 300
            except (AttributeError, OSError, comtypes.COMError):
                automation = comtypes.client.CreateObject(
                    client.CUIAutomation, interface=client.IUIAutomation
                )
            cache = automation.CreateCacheRequest()
            cache.TreeScope = client.TreeScope_Element
            cache.AutomationElementMode = client.AutomationElementMode_None
            cache.AddProperty(client.UIA_BoundingRectanglePropertyId)
            # Offscreen elements dominate deep trees. Discarding them here would
            # still pay to marshal each one across the process boundary, so the
            # provider filters instead: ~290ms drops to ~90ms on Chrome windows
            # for an identical set of on-screen rectangles.
            condition = automation.CreateAndCondition(
                automation.ControlViewCondition,
                automation.CreatePropertyCondition(
                    client.UIA_IsOffscreenPropertyId, False
                ),
            )
            self._automation, self._client = automation, client
            self._cache_request, self._condition = cache, condition
        except Exception:
            cache = automation = client = None
            self.close()
            raise

    def scan(self, hwnd: int) -> tuple[UIAElementSnapshot, ...]:
        root = self._automation.ElementFromHandle(hwnd)
        if not root:
            return ()
        elements = root.FindAllBuildCache(
            self._client.TreeScope_Descendants, self._condition, self._cache_request
        )
        snapshots = []
        for index in range(min(int(elements.Length), self.MAX_ELEMENTS)):
            try:
                rect = _rect_from_uia(
                    elements.GetElement(index).CachedBoundingRectangle
                )
                if rect:
                    snapshots.append(UIAElementSnapshot(rect, index))
            except Exception:
                # A disappearing child must not discard the other controls.
                continue
        return tuple(snapshots)

    def close(self):
        self._condition = self._cache_request = None
        self._automation = self._client = None
        if self._comtypes is not None:
            self._comtypes.CoUninitialize()
            self._comtypes = None


@dataclass
class _Session:
    """一次截图的收件箱。作废整个会话只要 set 一次，不用逐个撤请求。

    队列里可能同时躺着好几个属于上一次截图的请求，逐个取消要靠"取消动作跑在
    扫描开始之前"这个时序，请求一多就不牢靠。会话标记是 worker 取出任务时当场
    比对的，什么时候取消都算数。
    """

    replies: SimpleQueue = field(default_factory=SimpleQueue)
    cancelled: threading.Event = field(default_factory=threading.Event)


@dataclass
class _Request:
    hwnd: int
    session: _Session
    # 扫描一个窗口花了多久，只有日志会读。低配机器上这一项是判断"控件矩形
    # 为什么迟迟不出现"的唯一依据——提供方的耗时差着两个数量级。
    elapsed_ms: float = 0.0


class _ScanService:
    """进程级的固定 worker 池；没有 Qt 对象跨过这条边界。

    worker 数量是常量且随进程存活：每次截图新建的只是 UIAElementFinder，
    反复截图不会多出线程。提供方是跨进程同步调用，等待期间 GIL 是放开的，
    所以多一个 worker 能把几个窗口的等待重叠起来（实测 2 线程 1.74 倍，
    再加就到头了——UIA 客户端库内部有锁）。
    """

    WORKERS = 2

    def __init__(self, backend_factory=_UIABackend, workers=WORKERS):
        self._backend_factory = backend_factory
        self._condition = threading.Condition()
        self._queue = deque()
        self._closed = False
        self._threads = [
            threading.Thread(target=self._run, name=f"UIA detection {index}", daemon=True)
            for index in range(workers)
        ]
        for thread in self._threads:
            thread.start()

    def submit(self, request, *, urgent=False):
        """urgent 是鼠标正指着的窗口，插到队首；其余是预热，按加入顺序排。"""
        with self._condition:
            if self._closed or request.session.cancelled.is_set():
                return
            # 上一次截图取消后，它排的队还躺在这里；这里顺手清掉，免得
            # 疯狂截图-取消时队列只涨不消。
            if self._queue:
                self._queue = deque(
                    pending for pending in self._queue
                    if not pending.session.cancelled.is_set()
                )
            if urgent:
                self._queue.appendleft(request)
            else:
                self._queue.append(request)
            self._condition.notify()

    def _run(self):
        backend = None
        try:
            while True:
                with self._condition:
                    self._condition.wait_for(lambda: self._closed or self._queue)
                    if self._closed:
                        return
                    request = self._queue.popleft()
                if request.session.cancelled.is_set():
                    continue
                index, error = ElementIndex(), ""
                started = time.perf_counter()
                try:
                    if backend is None:
                        backend = self._backend_factory()
                    # 索引也在这里建：几千个矩形排序分桶要几毫秒，放到主线程
                    # 就是几个窗口的结果同时回来时的一次卡顿。
                    index = ElementIndex(backend.scan(request.hwnd))
                except Exception as exc:
                    error = str(exc)
                request.elapsed_ms = (time.perf_counter() - started) * 1000
                if not request.session.cancelled.is_set():
                    request.session.replies.put((request, index, error))
        finally:
            if backend is not None:
                backend.close()

    def close(self):
        # A stuck external provider must never hold up closing the screenshot
        # or the application. Each worker exits when its current call returns.
        with self._condition:
            self._closed = True
            self._queue.clear()
            self._condition.notify_all()


_service = None


def _scan_service():
    global _service
    if _service is None:
        _service = _ScanService()
        atexit.register(_service.close)
    return _service


class UIAElementFinder(QObject):
    """Per-capture cached rectangles; failed requests retain window fallback."""

    cache_updated = Signal(object)  # HWND is pointer-sized, not a Qt 32-bit int.

    # 预热的窗口数上限。扫描不只花自己的 CPU——COM 查询是让目标进程去构建
    # 无障碍树，多预热一个就多打扰一个用户没打算截的程序。而用户要截的几乎
    # 总是 Z 序最前的那一两个：靠后的窗口要截得先点它一下，点完它就排第一了。
    # 所以宁可少备几个，移到没预热的窗口无非是退回原来的等待。
    PREWARM_LIMIT = 3

    def __init__(self, parent=None, *, service=None):
        super().__init__(parent)
        self._service = service if service is not None else _scan_service()
        self._snapshots = {}
        self._session = _Session()
        self._inflight = set()
        self._closed = False
        self._last_error = ""
        self._poll_timer = QTimer(self)
        self._poll_timer.setInterval(33)
        self._poll_timer.timeout.connect(self._drain_results)

    def elements_at(
        self, hwnd: int, x: int, y: int, *, min_size=(1, 1)
    ) -> tuple[UIAElementSnapshot, ...]:
        index = self._snapshots.get(hwnd)
        return index.at(x, y, min_size=min_size) if index else ()

    def request_refresh_if_needed(self, hwnd: int):
        """鼠标正指着的窗口，插到队首。

        hwnd <= 0 是光标正从窗口之间的缝隙上划过。截图画面是冻结的，所以一个
        窗口的第一份快照服务整场截图，失败也算数，不重扫提供方。
        """
        if hwnd > 0:
            self._enqueue(hwnd, urgent=True)

    def prewarm(self, hwnds):
        """截图刚开始时把窗口按 Z 序排进队列。

        扫一个窗口要几十到上百毫秒，而且重复扫同一个窗口并不会变快（提供方每次
        都重新遍历），所以这笔钱省不掉，只能提前付：用户从按下快捷键到把鼠标移
        到目标窗口至少要几百毫秒，够把靠前的几个窗口扫完，移过去时结果已经在了。

        无响应的窗口在这里跳过、往后顺延。预热碰的是用户没有主动指定的窗口，
        为一个卡死的程序占住 worker 不值得；鼠标真指上去时仍然照常扫，那是
        用户自己的选择（见 request_refresh_if_needed）。
        """
        healthy = (hwnd for hwnd in hwnds if hwnd > 0 and not is_hung_window(hwnd))
        for hwnd in itertools.islice(healthy, self.PREWARM_LIMIT):
            self._enqueue(hwnd, urgent=False)

    def _enqueue(self, hwnd: int, *, urgent: bool):
        if self._closed or hwnd in self._snapshots or hwnd in self._inflight:
            return
        self._inflight.add(hwnd)
        self._service.submit(_Request(hwnd, self._session), urgent=urgent)
        self._poll_timer.start()

    def _drain_results(self):
        while not self._closed:
            try:
                request, index, error = self._session.replies.get_nowait()
            except Empty:
                break
            self._inflight.discard(request.hwnd)
            self._snapshots[request.hwnd] = index
            self._last_error = error
            log_debug(
                T("控件扫描耗时 {elapsed} ms，{count} 个元素{error}",
                  elapsed=round(request.elapsed_ms), count=len(index),
                  error=f": {error}" if error else ""),
                module="SmartSelection",
            )
            # 接收方可能就地关掉这次截图，剩下的结果连同定时器一起作废。
            self.cache_updated.emit(request.hwnd)
        if not self._inflight or self._closed:
            self._poll_timer.stop()

    def close(self):
        if self._closed:
            return
        self._closed = True
        self._poll_timer.stop()
        # 一次作废整个会话：已排队、正在扫、已扫完还没取走的结果全部不算数。
        self._session.cancelled.set()
        self._inflight.clear()
        self._snapshots.clear()
