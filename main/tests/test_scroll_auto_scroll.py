"""长截图的自动滚动：每步注入多少、什么时候截图、什么时候停。

AutoScroller 只和传进去的截图函数、注入函数、光标打交道，这里全换成假的；
最后一组用合成页面把整个长截图窗口跑一遍：自动滚到底自己停下，导出的长图与页面一致。
"""

import pytest
from PySide6.QtCore import QPoint

from stitch import auto_scroll
from stitch.auto_scroll import WHEEL_DELTA, AutoScroller
from stitch.incremental import FrameResult
from tests.long_capture_sim import PAGE_W, VIEW_H, FakeCursor, open_window, simulate, synthetic_page


class Rig:
    """假的截图与注入：记下每次注入的滚轮量，截图返回递增的帧序号。"""

    def __init__(self, capture_ok=True):
        self.injected = []
        self.captured = 0
        self.answered = 0
        self.capture_ok = capture_ok
        self.cursor = FakeCursor()
        self.reasons = []
        self.scroller = AutoScroller(self.capture, inject=self.inject, cursor=self.cursor)
        self.scroller.stopped.connect(self.reasons.append)

    def inject(self, delta, horizontal):
        self.injected.append((delta, horizontal))

    def capture(self, done):
        if not self.capture_ok:
            done(None)
            return
        self.captured += 1
        done(self.captured)

    def result(self, top, ok=True, shift=0, index=None):
        index = self.captured if index is None else index
        self.scroller.on_frame(FrameResult(ok, False, index, 100, 1000, 0, None, top=top, shift=shift))

    def feed(self, qtbot, tops):
        """按帧序号依次等每一帧截下来，再交回它的拼接结论。"""
        for top in tops:
            index = self.answered + 1
            qtbot.waitUntil(lambda index=index: self.captured >= index)
            self.result(top=top, index=index)
            self.answered = index


@pytest.fixture(autouse=True)
def fast_timers(monkeypatch):
    monkeypatch.setattr(auto_scroll, "SETTLE_MS", 1)
    monkeypatch.setattr(auto_scroll, "STEP_INTERVAL_MS", 1)
    monkeypatch.setattr(auto_scroll, "POLL_MS", 5)


@pytest.fixture
def rig(qapp):
    rig = Rig()
    rig.scroller.on_frame(FrameResult(True, True, 0, 100, 600, 0, None))  # 进长截图时的第一帧
    yield rig
    rig.scroller.stop()


def _start(rig, horizontal=False, forward=True):
    rig.scroller.start(QPoint(300, 400), horizontal, forward)


def test_start_parks_the_cursor_and_scrolls_one_notch(qtbot, rig):
    _start(rig)
    assert rig.cursor.position == QPoint(300, 400)
    assert rig.injected == [(-WHEEL_DELTA, False)]  # 竖向往下：滚轮负值
    qtbot.waitUntil(lambda: rig.captured == 1)


def test_next_step_waits_for_the_previous_frame(qtbot, rig):
    _start(rig)
    qtbot.waitUntil(lambda: rig.captured == 1)
    qtbot.wait(20)
    assert len(rig.injected) == 1
    rig.result(top=20)
    qtbot.waitUntil(lambda: len(rig.injected) == 2)
    assert rig.injected[-1] == (-WHEEL_DELTA, False)


def test_steps_keep_their_interval_even_when_frames_come_back_fast(qtbot, monkeypatch, rig):
    monkeypatch.setattr(auto_scroll, "STEP_INTERVAL_MS", 150)
    _start(rig)
    qtbot.waitUntil(lambda: rig.captured == 1)
    rig.result(top=20)
    assert len(rig.injected) == 1
    qtbot.waitUntil(lambda: len(rig.injected) == 2, timeout=1000)


def test_stops_after_three_steps_without_moving(qtbot, rig):
    _start(rig)
    rig.feed(qtbot, [20, 20, 20])
    assert rig.scroller.running
    rig.feed(qtbot, [20])
    assert rig.reasons == ["end"]


def test_a_page_that_starts_late_is_not_at_the_end(qtbot, rig):
    """页面偶尔晚一两步才开始滚，不能当成到底。"""
    _start(rig)
    rig.feed(qtbot, [0, 0, 60, 120, 180])
    assert rig.scroller.running


def test_growing_the_top_counts_as_moving(qtbot, rig):
    """往上越过起点时这一帧的 top 一直是 0，挪动体现在 shift 里，不能当成到底。"""
    _start(rig, forward=False)
    assert rig.injected == [(WHEEL_DELTA, False)]
    for _ in range(10):
        expected = rig.captured + 1
        qtbot.waitUntil(lambda expected=expected: rig.captured == expected)
        rig.result(top=0, shift=20)
    assert rig.scroller.running


def test_moving_the_mouse_stops_it(qtbot, rig):
    _start(rig)
    rig.cursor.position = QPoint(310, 400)
    qtbot.waitUntil(lambda: not rig.scroller.running)
    assert rig.reasons == ["mouse"]


def test_step_scrolled_before_the_mouse_moved_is_still_captured(qtbot, monkeypatch, rig):
    """停下时已经滚出去的那一步照常截，长图才和屏幕上停住的位置一致；之后不再滚。"""
    monkeypatch.setattr(auto_scroll, "SETTLE_MS", 80)
    _start(rig)
    rig.cursor.position = QPoint(310, 400)
    qtbot.waitUntil(lambda: rig.reasons == ["mouse"])
    assert rig.captured == 0
    qtbot.waitUntil(lambda: rig.captured == 1)
    rig.result(top=20)
    qtbot.wait(50)
    assert rig.injected == [(-WHEEL_DELTA, False)]


def test_flush_captures_the_pending_step_now(qapp, monkeypatch, rig):
    monkeypatch.setattr(auto_scroll, "SETTLE_MS", 10_000)
    _start(rig)
    rig.scroller.stop()
    rig.scroller.flush()
    assert rig.captured == 1


def test_cancel_drops_the_pending_step(qtbot, monkeypatch, rig):
    monkeypatch.setattr(auto_scroll, "SETTLE_MS", 30)
    _start(rig)
    rig.scroller.cancel()
    qtbot.wait(80)
    assert rig.captured == 0
    assert rig.reasons == ["user"]


def test_unmatched_frame_stops_it(qtbot, rig):
    _start(rig)
    qtbot.waitUntil(lambda: rig.captured == 1)
    rig.result(top=0, ok=False)
    assert rig.reasons == ["failed"]


def test_failed_capture_stops_it(qtbot, qapp):
    rig = Rig(capture_ok=False)
    _start(rig)
    qtbot.waitUntil(lambda: not rig.scroller.running)
    assert rig.reasons == ["failed"]


def test_results_of_other_frames_only_track_the_position(qtbot, rig):
    """自动滚动开始前截的帧晚到，只更新位置，不推动下一步。"""
    _start(rig)
    qtbot.waitUntil(lambda: rig.captured == 1)
    rig.result(top=90, index=0)
    qtbot.wait(20)
    assert len(rig.injected) == 1
    rig.result(top=110)
    qtbot.waitUntil(lambda: len(rig.injected) == 2)


@pytest.mark.parametrize("horizontal, forward, sign", [
    (False, True, -1), (False, False, 1), (True, True, 1), (True, False, -1),
])
def test_wheel_direction(qapp, horizontal, forward, sign):
    rig = Rig()
    _start(rig, horizontal, forward)
    assert rig.injected == [(sign * WHEEL_DELTA, horizontal)]
    rig.scroller.stop()
    assert rig.reasons == ["user"]


# ---- 整个长截图窗口：合成页面上自动滚到底 ----


@pytest.fixture
def window(qapp, monkeypatch):
    win = open_window(monkeypatch)
    yield win
    if win._stitcher is not None:
        win._cleanup()


def _drive(qtbot, monkeypatch, win, page, top, last_heading=None, mode="smooth", unsettled=0, hover=False):
    sim = simulate(monkeypatch, win, page, top, mode, unsettled, hover)
    reasons = []
    win._auto_scroller.stopped.connect(reasons.append)
    win._do_capture()  # 进长截图时的第一帧
    win._last_heading = last_heading
    win._toggle_auto_scroll()
    assert win.toolbar.auto_scroll_btn.isChecked()
    qtbot.waitUntil(lambda: bool(reasons), timeout=30000)
    assert not win.toolbar.auto_scroll_btn.isChecked()
    win._on_finish()
    return reasons, sim, win.captured["image"]


@pytest.mark.parametrize("mode", ["smooth", "accumulate", "whole"])
def test_auto_scroll_runs_to_the_end_of_the_page(qtbot, monkeypatch, window, mode):
    page = synthetic_page(1600)
    reasons, sim, image = _drive(qtbot, monkeypatch, window, page, 0, mode=mode)
    assert reasons == ["end"]
    assert sim.top == page.height - VIEW_H
    assert image.convert("RGB").tobytes() == page.tobytes()
    assert set(sim.injected) == {-WHEEL_DELTA}


def test_auto_scroll_keeps_going_up_after_scrolling_up(qtbot, monkeypatch, window):
    page = synthetic_page(1600, seed=6)
    reasons, sim, image = _drive(qtbot, monkeypatch, window, page, 900, last_heading="up")
    assert reasons == ["end"]
    assert sim.top == 0
    assert image.convert("RGB").tobytes() == page.crop((0, 0, PAGE_W, 900 + VIEW_H)).tobytes()


def test_frames_are_taken_once_the_page_stops_changing(qtbot, monkeypatch, window):
    """平滑滚动的程序在动画中途常有一块还没重绘完：只把停住的画面交给拼接。"""
    submitted = []
    submit = window._submit_frame
    monkeypatch.setattr(window, "_submit_frame", lambda frame: (submitted.append(hash(frame[0])), submit(frame))[1])
    page = synthetic_page(1600, seed=12)
    reasons, sim, image = _drive(qtbot, monkeypatch, window, page, 0, unsettled=2)
    assert reasons == ["end"]
    assert sim.torn and len(submitted) > 20
    assert not sim.torn & set(submitted)
    assert image.convert("RGB").tobytes() == page.tobytes()


@pytest.mark.parametrize("top, heading", [(0, None), (900, "up")])
def test_hover_highlight_under_the_parked_cursor_stays_out(qtbot, monkeypatch, window, top, heading):
    """光标停在截图区中间，每帧中间那一行都被悬停高亮；高亮落在重叠区里，不能被拼进长图。"""
    page = synthetic_page(1600, seed=14)
    reasons, sim, image = _drive(qtbot, monkeypatch, window, page, top, last_heading=heading, hover=True)
    assert reasons == ["end"]
    expected = page if heading is None else page.crop((0, 0, PAGE_W, top + VIEW_H))
    assert image.convert("RGB").tobytes() == expected.tobytes()


@pytest.mark.parametrize("follows_focus", [True, False])
def test_target_window_gets_focus_when_the_wheel_follows_focus(qtbot, monkeypatch, window, follows_focus):
    """关掉了“悬停时滚动非活动窗口”时滚轮发给有焦点的窗口，开始前先把截图区中心下面的窗口设为前台。"""
    from stitch import scroll_window
    activated = []
    monkeypatch.setattr(scroll_window, "wheel_follows_focus", lambda: follows_focus)
    monkeypatch.setattr(scroll_window, "activate_window_at", lambda x, y: activated.append((x, y)))
    simulate(monkeypatch, window, synthetic_page(1600), 0)
    window._do_capture()
    window._toggle_auto_scroll()
    center = window.capture_rect.center()
    assert activated == ([(center.x(), center.y())] if follows_focus else [])
    window._auto_scroller.cancel()
