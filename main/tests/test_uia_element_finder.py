"""Element picking, provider failures and capture-session cancellation."""

from queue import Empty
import threading
from types import SimpleNamespace

import pytest

from capture.uia_element_finder import (
    ElementIndex, UIAElementFinder, UIAElementSnapshot, _Request, _ScanService,
    _Session, _UIABackend, _rect_from_uia,
)


def test_nested_controls_and_adjacent_edges_chain_from_smallest_outwards():
    panel = UIAElementSnapshot((-100, -50, 200, 200))
    first = UIAElementSnapshot((10, 10, 15, 20), 1)
    second = UIAElementSnapshot((15, 10, 20, 20), 2)
    index = ElementIndex((panel, first, second))
    assert index.at(14, 12) == (first, panel)
    assert index.at(15, 12) == (second, panel)
    assert index.at(-90, -40) == (panel,)
    assert index.at(250, 0) == ()


def test_index_finds_elements_spanning_many_buckets_and_negative_coordinates():
    """分桶不能改变命中集合：跨多个桶的大矩形和负坐标显示器都要照常命中。"""
    tall = UIAElementSnapshot((-2000, -1000, 2000, 1000))
    small = UIAElementSnapshot((-1900, -900, -1800, -800), 1)
    index = ElementIndex((tall, small))
    assert index.at(-1850, -850) == (small, tall)
    assert index.at(0, 0) == (tall,)
    assert index.at(-1850, 0) == (tall,)
    assert len(index) == 2


@pytest.mark.parametrize("coords", [(0, 0, 0, 2), (2, 0, 1, 2), (0, 5, 2, 3)])
def test_invalid_provider_rectangles_are_discarded(coords):
    assert _rect_from_uia(SimpleNamespace(**dict(zip(("left", "top", "right", "bottom"), coords)))) is None


def test_snapshot_reads_cached_rectangles_and_skips_broken_children():
    def element(rect):
        return SimpleNamespace(CachedBoundingRectangle=SimpleNamespace(
            **dict(zip(("left", "top", "right", "bottom"), rect))
        ))

    children = [element((1, 2, 10, 20)), element((0, 0, 0, 0)), SimpleNamespace()]
    calls, searches = [], []
    root = SimpleNamespace(FindAllBuildCache=lambda *args: searches.append(args) or SimpleNamespace(
        Length=len(children), GetElement=lambda index: children[index]
    ))
    backend = _UIABackend.__new__(_UIABackend)
    backend._automation = SimpleNamespace(
        ElementFromHandle=lambda hwnd: calls.append(hwnd) or root
    )
    backend._client = SimpleNamespace(TreeScope_Descendants=4)
    backend._cache_request, backend._condition = object(), object()
    assert backend.scan(2**34) == (UIAElementSnapshot((1, 2, 10, 20)),)
    assert calls == [2**34]
    # 离屏元素交给提供方过滤，客户端不再读 IsOffscreen：深树上这两种做法
    # 结果相同，但客户端过滤要先为每个离屏元素付一次跨进程封送。
    assert searches == [(4, backend._condition, backend._cache_request)]


class RecordingService:
    """记下 submit 了什么，让测试自己决定哪一条什么时候完成。"""

    def __init__(self):
        self.requests = []
        self.urgent = []

    def submit(self, request, *, urgent=False):
        self.requests.append(request)
        self.urgent.append(urgent)

    def complete(self, index=-1, snapshots=(), error=""):
        request = self.requests[index]
        request.session.replies.put((request, ElementIndex(snapshots), error))


@pytest.fixture
def finder(qapp):
    instance = UIAElementFinder(service=RecordingService())
    yield instance
    instance.close()
    instance.deleteLater()


def test_first_hover_returns_immediately_then_uses_one_snapshot_per_capture(finder):
    finder.request_refresh_if_needed(100)
    finder.request_refresh_if_needed(100)
    assert finder.elements_at(100, 5, 5) == ()
    assert len(finder._service.requests) == 1
    rect = UIAElementSnapshot((0, 0, 10, 10))
    finder._service.complete(snapshots=(rect,))
    finder._drain_results()
    finder.request_refresh_if_needed(100)
    assert finder.elements_at(100, 5, 5) == (rect,)
    assert len(finder._service.requests) == 1
    assert not finder._poll_timer.isActive()


def test_hovered_window_jumps_the_queue_ahead_of_prewarmed_ones(finder):
    finder.prewarm([100, 200, 300])
    finder.request_refresh_if_needed(400)
    assert [r.hwnd for r in finder._service.requests] == [100, 200, 300, 400]
    assert finder._service.urgent == [False, False, False, True]


def test_prewarm_skips_what_is_already_queued_or_cached(finder):
    finder.request_refresh_if_needed(100)
    finder._service.complete(snapshots=(UIAElementSnapshot((0, 0, 10, 10)),))
    finder._drain_results()
    finder.request_refresh_if_needed(200)
    finder.prewarm([100, 200, 300])
    assert [r.hwnd for r in finder._service.requests] == [100, 200, 300]


def test_prewarm_stops_at_the_limit(finder):
    finder.prewarm(range(1, 100))
    assert len(finder._service.requests) == UIAElementFinder.PREWARM_LIMIT


def test_prewarm_skips_hung_windows_and_takes_the_next_one_instead(finder, monkeypatch):
    """卡死的程序不该占住 worker，预热往后顺延——但名额不能因此少掉。"""
    monkeypatch.setattr("capture.uia_element_finder.is_hung_window",
                        lambda hwnd: hwnd in {2, 3})
    finder.prewarm(range(1, 100))
    queued = [r.hwnd for r in finder._service.requests]
    assert 2 not in queued and 3 not in queued
    assert len(queued) == UIAElementFinder.PREWARM_LIMIT
    assert queued[:3] == [1, 4, 5]


def test_hovered_window_is_scanned_even_when_it_is_hung(finder, monkeypatch):
    """用户主动指上去的窗口照常扫，卡不卡是他自己的选择。"""
    monkeypatch.setattr("capture.uia_element_finder.is_hung_window", lambda hwnd: True)
    finder.request_refresh_if_needed(100)
    assert [r.hwnd for r in finder._service.requests] == [100]


def test_several_windows_land_in_the_cache_and_each_announces_itself(finder):
    updated = []
    finder.cache_updated.connect(updated.append)
    finder.prewarm([100, 200])
    finder._service.complete(0, (UIAElementSnapshot((0, 0, 100, 100)),))
    finder._service.complete(1, (UIAElementSnapshot((1, 2, 10, 20)),))
    finder._drain_results()
    assert updated == [100, 200]
    assert finder.elements_at(100, 5, 5)[0].rect == (0, 0, 100, 100)
    assert finder.elements_at(200, 5, 5)[0].rect == (1, 2, 10, 20)
    assert not finder._poll_timer.isActive()


def test_poll_timer_keeps_running_until_the_last_scan_lands(finder):
    finder.prewarm([100, 200])
    finder._service.complete(0)
    finder._drain_results()
    assert finder._poll_timer.isActive()
    finder._service.complete(1)
    finder._drain_results()
    assert not finder._poll_timer.isActive()


def test_provider_failure_does_not_retry_on_every_mouse_move(finder):
    finder.request_refresh_if_needed(100)
    finder._service.complete(error="provider unavailable")
    finder._drain_results()
    for _ in range(100):
        finder.request_refresh_if_needed(100)
        assert finder.elements_at(100, 5, 5) == ()
    assert len(finder._service.requests) == 1
    assert finder._last_error == "provider unavailable"


def test_crossing_a_gap_between_windows_keeps_the_running_scan(finder):
    finder.request_refresh_if_needed(100)
    finder.request_refresh_if_needed(0)
    assert [r.hwnd for r in finder._service.requests] == [100]
    assert finder._poll_timer.isActive()
    finder._service.complete(snapshots=(UIAElementSnapshot((0, 0, 10, 10)),))
    finder._drain_results()
    assert finder.elements_at(100, 5, 5)[0].rect == (0, 0, 10, 10)


def test_closing_voids_the_whole_session_in_one_go(finder):
    """取消一次截图不该依赖逐个撤请求——会话标记一次作废全部。"""
    updated = []
    finder.cache_updated.connect(updated.append)
    finder.prewarm([100, 200, 300])
    session = finder._session
    finder.close()
    assert session.cancelled.is_set()
    for index in range(3):
        finder._service.complete(index, (UIAElementSnapshot((0, 0, 100, 100)),))
    finder._drain_results()
    assert updated == []
    assert not finder._poll_timer.isActive()


def test_a_new_capture_never_sees_the_previous_sessions_results(qapp):
    """上一次截图排的队还在服务里躺着时，新截图必须拿不到它的结果。"""
    service = RecordingService()
    old = UIAElementFinder(service=service)
    old.prewarm([100, 200])
    old.close()
    new = UIAElementFinder(service=service)
    try:
        assert new._session is not old._session
        # 旧会话的请求这时候才完成，收件箱是旧的，新会话不该受影响
        service.complete(0, (UIAElementSnapshot((0, 0, 100, 100)),))
        new._drain_results()
        assert new.elements_at(100, 5, 5) == ()
        new.request_refresh_if_needed(100)
        service.complete(snapshots=(UIAElementSnapshot((5, 5, 50, 50)),))
        new._drain_results()
        assert new.elements_at(100, 6, 6)[0].rect == (5, 5, 50, 50)
    finally:
        new.close()
        new.deleteLater()
        old.deleteLater()


def test_handler_closing_the_capture_stops_the_remaining_results(finder):
    """cache_updated 的接收方就地关掉截图时，剩下的结果连同定时器一起作废。"""
    updated = []
    finder.cache_updated.connect(lambda hwnd: (updated.append(hwnd), finder.close()))
    finder.prewarm([100, 200])
    finder._service.complete(0)
    finder._service.complete(1)
    finder._drain_results()
    assert updated == [100]
    assert not finder._poll_timer.isActive()


# ============================================================================
# 扫描服务：线程上界、并发、关闭
# ============================================================================

def _blocking_backend(entered, release, calls):
    """扫到 hwnd == 1 就停住，用来把 worker 占住、观察队列怎么排。"""
    class Backend:
        def __init__(self):
            calls.append(("init", threading.get_ident()))

        def scan(self, hwnd):
            calls.append((hwnd, threading.get_ident()))
            if hwnd == 1:
                entered.set()
                assert release.wait(3)
            return ()

        def close(self):
            calls.append(("close", threading.get_ident()))
    return Backend


def _idle_backend():
    return SimpleNamespace(scan=lambda hwnd: (), close=lambda: None)


def test_worker_count_is_fixed_and_does_not_grow_with_submissions():
    """疯狂截图-取消不该多出线程：worker 数由常量定，和 submit 次数无关。"""
    before = threading.active_count()
    service = _ScanService(_idle_backend, workers=2)
    try:
        assert threading.active_count() == before + 2
        for _ in range(200):
            session = _Session()
            service.submit(_Request(1, session))
            session.cancelled.set()
        assert threading.active_count() == before + 2
        assert len(service._threads) == 2
    finally:
        service.close()
        for thread in service._threads:
            thread.join(3)
    assert threading.active_count() == before


def test_cancelled_sessions_do_not_pile_up_in_the_queue():
    """取消的会话留在队列里只会越积越多，submit 时要顺手清掉。"""
    entered, release = threading.Event(), threading.Event()
    service = _ScanService(_blocking_backend(entered, release, []), workers=1)
    try:
        blocking = _Session()
        service.submit(_Request(1, blocking))
        assert entered.wait(3)
        for _ in range(50):
            dead = _Session()
            service.submit(_Request(2, dead))
            dead.cancelled.set()
        live = _Session()
        service.submit(_Request(3, live))
        assert [r.hwnd for r in service._queue] == [3]
    finally:
        release.set()
        service.close()
        for thread in service._threads:
            thread.join(3)


def test_workers_run_in_parallel_on_their_own_backends():
    """两个 worker 必须真的同时在跑，否则并行扫描没有意义。"""
    both_in = threading.Barrier(2, timeout=3)
    idents = []

    class Backend:
        def scan(self, hwnd):
            idents.append(threading.get_ident())
            both_in.wait()          # 两个都进来才放行，串行执行会在这里超时
            return ()

        def close(self):
            pass

    service = _ScanService(Backend, workers=2)
    session = _Session()
    try:
        service.submit(_Request(1, session))
        service.submit(_Request(2, session))
        for _ in range(2):
            session.replies.get(timeout=3)
        assert len(set(idents)) == 2
        assert threading.get_ident() not in idents
    finally:
        service.close()
        for thread in service._threads:
            thread.join(3)


def test_urgent_request_is_taken_before_everything_already_queued():
    entered, release = threading.Event(), threading.Event()
    calls = []
    service = _ScanService(_blocking_backend(entered, release, calls), workers=1)
    session = _Session()
    try:
        service.submit(_Request(1, session))                # 占住唯一的 worker
        assert entered.wait(3)
        service.submit(_Request(2, session))                # 预热
        service.submit(_Request(3, session))                # 预热
        service.submit(_Request(4, session), urgent=True)   # 鼠标指着的
        release.set()
        taken = [session.replies.get(timeout=3)[0].hwnd for _ in range(4)]
        assert taken == [1, 4, 2, 3]
    finally:
        release.set()
        service.close()
        for thread in service._threads:
            thread.join(3)


def test_backend_is_created_and_released_on_the_worker_thread():
    entered, release = threading.Event(), threading.Event()
    calls = []
    service = _ScanService(_blocking_backend(entered, release, calls), workers=1)
    session = _Session()
    try:
        service.submit(_Request(1, session))
        assert entered.wait(3)
        release.set()
        session.replies.get(timeout=3)
    finally:
        release.set()
        service.close()
        for thread in service._threads:
            thread.join(3)
    assert [call[0] for call in calls] == ["init", 1, "close"]
    assert len({call[1] for call in calls}) == 1
    assert calls[0][1] != threading.get_ident()


def test_cancelled_request_is_dropped_before_the_provider_is_called():
    calls = []
    service = _ScanService(
        lambda: SimpleNamespace(scan=lambda hwnd: calls.append(hwnd) or (),
                                close=lambda: None),
        workers=1)
    session = _Session()
    session.cancelled.set()
    service.submit(_Request(1, session))
    service.close()
    for thread in service._threads:
        thread.join(3)
    assert calls == []


def test_worker_close_does_not_wait_for_blocked_provider():
    entered, release = threading.Event(), threading.Event()

    class Backend:
        def scan(self, hwnd):
            entered.set()
            assert release.wait(3)
            return ()

        def close(self):
            pass

    service = _ScanService(Backend, workers=1)
    session = _Session()
    try:
        service.submit(_Request(1, session))
        assert entered.wait(3)
        session.cancelled.set()
        service.close()
        assert service._threads[0].is_alive()
        assert service._threads[0].daemon
    finally:
        release.set()
        service.close()
        for thread in service._threads:
            thread.join(3)
    with pytest.raises(Empty):
        session.replies.get_nowait()
