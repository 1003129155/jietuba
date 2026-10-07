"""拼接接不上时，预览面板顶部写出原因和办法；下一帧接上后提示消失。

孤立的一张怪帧（闪白、动画中途）之后马上有好帧接上，提示不出现；连续接不上只记一条日志。
"""

import random

import pytest
from PySide6.QtGui import QImage

from stitch import scroll_window
from tests.long_capture_sim import PAGE_W, VIEW_H, open_window, simulate, synthetic_page


@pytest.fixture
def window(qapp, monkeypatch):
    monkeypatch.setattr(scroll_window, "WARNING_DELAY_MS", 50)
    win = open_window(monkeypatch)
    yield win
    if win._stitcher is not None:
        win._cleanup()


def _noise():
    rnd = random.Random(5)
    data = bytes(rnd.randrange(256) for _ in range(PAGE_W * VIEW_H * 4))
    return QImage(data, PAGE_W, VIEW_H, PAGE_W * 4, QImage.Format.Format_RGB32).copy()


def test_unmatched_frame_shows_the_hint_until_the_next_match(qtbot, monkeypatch, window):
    sim = simulate(monkeypatch, window, synthetic_page(1600, seed=3), 0)
    window._do_capture()
    sim.scroll_and_capture(-3)
    monkeypatch.setattr(window, "_grab_capture_rect", _noise)
    window._do_capture()
    qtbot.waitUntil(lambda: window.preview_warning_active)
    label = window.preview_panel.warning_label
    assert not label.isHidden()
    assert label.text() == window.tr("Couldn't join. Scroll back")

    monkeypatch.setattr(window, "_grab_capture_rect", sim.grab)
    sim.scroll_and_capture(-3)
    qtbot.waitUntil(lambda: not window.preview_warning_active)
    assert label.isHidden()


def test_a_lone_unmatched_frame_does_not_flash_the_hint(qtbot, monkeypatch, window):
    monkeypatch.setattr(scroll_window, "WARNING_DELAY_MS", 400)
    shown = []
    monkeypatch.setattr(window.preview_panel, "show_warning", shown.append)
    sim = simulate(monkeypatch, window, synthetic_page(1600, seed=3), 0)
    window._do_capture()
    sim.scroll_and_capture(-3)
    monkeypatch.setattr(window, "_grab_capture_rect", _noise)
    window._do_capture()
    qtbot.waitUntil(lambda: window._warning_timer.isActive())

    monkeypatch.setattr(window, "_grab_capture_rect", sim.grab)
    sim.scroll_and_capture(-3)
    qtbot.waitUntil(lambda: not window._warning_timer.isActive())
    qtbot.wait(500)
    assert shown == []
    assert not window.preview_warning_active


def test_auto_scroll_stopped_by_an_unmatched_frame_shows_the_hint_at_once(qtbot, monkeypatch, window):
    monkeypatch.setattr(scroll_window, "WARNING_DELAY_MS", 60_000)
    simulate(monkeypatch, window, synthetic_page(1600, seed=3), 0)
    window._do_capture()
    monkeypatch.setattr(window, "_grab_capture_rect", _noise)
    reasons = []
    window._auto_scroller.stopped.connect(reasons.append)
    window._toggle_auto_scroll()
    qtbot.waitUntil(lambda: bool(reasons))
    assert reasons == ["failed"]
    assert window.preview_warning_active
    assert not window.preview_panel.warning_label.isHidden()


def test_consecutive_unmatched_frames_log_once_at_debug_level(qtbot, monkeypatch, window):
    logged = []
    monkeypatch.setattr(scroll_window, "_log_stitch",
                        lambda *args, force=False: logged.append((getattr(args[0], "template", args[0]), force)))
    sim = simulate(monkeypatch, window, synthetic_page(1600, seed=3), 0)
    window._do_capture()
    sim.scroll_and_capture(-3)
    monkeypatch.setattr(window, "_grab_capture_rect", _noise)
    for _ in range(3):
        window._do_capture()
    qtbot.waitUntil(lambda: window._warning_timer.isActive() or window.preview_warning_active)
    qtbot.wait(100)
    unmatched = [entry for entry in logged if "未找到重叠区域" in entry[0]]
    assert unmatched == [(unmatched[0][0], False)]

    monkeypatch.setattr(window, "_grab_capture_rect", sim.grab)
    sim.scroll_and_capture(-3)
    qtbot.waitUntil(lambda: not window.preview_warning_active and not window._warning_timer.isActive())
    monkeypatch.setattr(window, "_grab_capture_rect", _noise)
    window._do_capture()
    qtbot.waitUntil(lambda: len([e for e in logged if "未找到重叠区域" in e[0]]) == 2)
