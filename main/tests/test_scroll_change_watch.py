"""长截图的画面变化监视：后台发现截图区变了就通知窗口，窗口等画面停住再截。

后台截屏换成按脚本回答“变没变”的假区域，窗口截的是合成页面，不碰真实屏幕。
"""

import threading
import time

import pytest
from PIL import Image
from PySide6.QtCore import QRect
from PySide6.QtGui import QImage

from stitch import change_watch, scroll_window
from stitch.change_watch import ChangeWatch
from stitch.scroll_window import ScrollCaptureWindow
from tests.long_capture_sim import PAGE_W, VIEW_H, open_window, simulate, synthetic_page

_capture_initial_screenshot = ScrollCaptureWindow._capture_initial_screenshot  # open_window 会把它换掉


class FakeRegion:
    """按 answers 依次回答变没变，答完一直没变；记下每次看的时间和在哪个线程关闭。"""

    def __init__(self, answers=()):
        self.answers = list(answers)
        self.calls = []
        self.opened_on = None
        self.closed_on = None

    def __call__(self, rect):
        self.opened_on = threading.current_thread()
        return self

    def changed(self):
        self.calls.append(time.monotonic())
        return self.answers.pop(0) if self.answers else False

    def close(self):
        self.closed_on = threading.current_thread()


def _watch(region):
    watch = ChangeWatch(QRect(0, 0, 10, 10), region=region)
    reports = []
    watch.changed.connect(lambda: reports.append(threading.current_thread()))
    return watch, reports


def test_changes_are_reported_on_the_ui_thread(qtbot):
    region = FakeRegion([False, True, False, True])
    watch, reports = _watch(region)
    watch.start()
    qtbot.waitUntil(lambda: len(reports) == 2)
    watch.stop()
    assert reports == [threading.main_thread()] * 2
    assert region.opened_on is region.closed_on is not threading.main_thread()


def test_stop_waits_for_the_thread_and_nothing_follows(qtbot):
    region = FakeRegion()
    watch, reports = _watch(region)
    watch.start()
    qtbot.waitUntil(lambda: len(region.calls) >= 2)
    watch.stop()
    assert region.closed_on is not None
    calls = len(region.calls)
    qtbot.wait(80)
    assert len(region.calls) == calls and reports == []


def test_it_looks_more_often_while_the_screen_changes(qtbot, monkeypatch):
    monkeypatch.setattr(change_watch, "IDLE_MS", 200)
    monkeypatch.setattr(change_watch, "ACTIVE_MS", 20)
    monkeypatch.setattr(change_watch, "ACTIVE_HOLD_MS", 300)
    region = FakeRegion([False, False, True])
    watch, _ = _watch(region)
    watch.start()
    qtbot.waitUntil(lambda: len(region.calls) >= 3, timeout=3000)
    changed_at = region.calls[2]
    qtbot.waitUntil(lambda: region.calls[-1] - changed_at > 0.6, timeout=3000)
    watch.stop()
    gaps = [(b - a, a - changed_at) for a, b in zip(region.calls, region.calls[1:])]
    idle_before = [gap for gap, since in gaps if since < 0]
    active = [gap for gap, since in gaps if 0 <= since < 0.25]
    idle_after = [gap for gap, since in gaps if since >= 0.35]
    assert idle_before and min(idle_before) > 0.15
    assert len(active) >= 3 and max(active) < 0.1
    assert idle_after and min(idle_after) > 0.15


# ---- 窗口：变化通知之后怎么截 ----


@pytest.fixture
def window(qapp, monkeypatch):
    win = open_window(monkeypatch)
    yield win
    if win._stitcher is not None:
        win._cleanup()


def test_a_change_is_captured_once_the_page_stops(qtbot, monkeypatch, window):
    sim = simulate(monkeypatch, window, synthetic_page(1600, seed=21), 0)
    window._do_capture()
    sim.inject(-3 * 120)
    window._on_screen_changed()
    window._on_screen_changed()  # 还在等停住时再来的通知不另截
    qtbot.waitUntil(lambda: not window._still_timer.isActive())
    assert len(window.screenshots) == 2
    window._on_finish()
    assert window.captured["image"].convert("RGB").tobytes() == sim.page.crop((0, 0, PAGE_W, 120 + VIEW_H)).tobytes()


def test_changes_during_auto_scroll_are_left_to_it(qtbot, monkeypatch, window):
    simulate(monkeypatch, window, synthetic_page(1600), 0)
    window._do_capture()
    monkeypatch.setattr(type(window._auto_scroller), "running", property(lambda self: True))
    window._on_screen_changed()
    assert not window._still_timer.isActive()
    assert len(window.screenshots) == 1


def test_a_blank_screen_is_not_taken_as_still(qtbot, monkeypatch, window):
    """整屏刷白时连截两次也一样，但没有能拼的内容：等它画出来再交。"""
    sim = simulate(monkeypatch, window, synthetic_page(1600, seed=22), 0)
    window._do_capture()
    sim.inject(-3 * 120)
    white = Image.new("RGBA", (PAGE_W, VIEW_H), "white").tobytes("raw", "BGRA")
    blank = QImage(white, PAGE_W, VIEW_H, PAGE_W * 4, QImage.Format.Format_RGB32).copy()
    pending = [blank, blank]
    content = sim.grab

    def grab():
        return pending.pop(0) if pending else content()

    monkeypatch.setattr(window, "_grab_capture_rect", grab)
    submitted = []
    submit = window._submit_frame
    monkeypatch.setattr(window, "_submit_frame", lambda frame: (submitted.append(frame[0]), submit(frame))[1])
    window._capture_when_still()
    qtbot.waitUntil(lambda: bool(submitted))
    expected = sim.page.crop((0, 120, PAGE_W, 120 + VIEW_H)).convert("RGBA").tobytes("raw", "BGRA")
    assert submitted == [expected]


@pytest.mark.parametrize("direction, tries", [("vertical", scroll_window.STILL_CHECK_TRIES), ("horizontal", 2)])
def test_stripes_along_the_scroll_axis_count_as_content(qtbot, monkeypatch, window, direction, tries):
    """竖条纹每一行都一样：竖向拼不了，不算停住，等满次数才交；横向拼的是列，连截两次一样就交。"""
    row = b"".join(bytes(((x * 37) % 256, (x * 11) % 256, (x * 5) % 256, 255)) for x in range(PAGE_W))
    stripes = QImage(row * VIEW_H, PAGE_W, VIEW_H, PAGE_W * 4, QImage.Format.Format_RGB32).copy()
    monkeypatch.setattr(window, "_grab_capture_rect", lambda: stripes)
    window.scroll_direction = direction
    submitted = []
    monkeypatch.setattr(window, "_submit_frame", lambda frame: submitted.append(window._still_tries))
    window._capture_when_still()
    qtbot.waitUntil(lambda: bool(submitted))
    assert submitted == [tries]


def test_finishing_takes_the_view_the_watch_has_not_noticed_yet(qtbot, monkeypatch, window):
    sim = simulate(monkeypatch, window, synthetic_page(1600, seed=23), 0)
    window._do_capture()
    sim.inject(-3 * 120)  # 刚滚完，画面监视还没来得及看
    window._on_finish()
    assert window.captured["image"].convert("RGB").tobytes() == sim.page.crop((0, 0, PAGE_W, 120 + VIEW_H)).tobytes()


def test_the_watch_runs_from_the_first_frame_until_cleanup(qtbot, monkeypatch, window):
    simulate(monkeypatch, window, synthetic_page(1600), 0)
    region = FakeRegion()
    window._change_watch._region = region
    _capture_initial_screenshot(window)
    assert len(window.screenshots) == 1
    qtbot.waitUntil(lambda: bool(region.calls))
    window._cleanup()
    assert region.closed_on is not None
