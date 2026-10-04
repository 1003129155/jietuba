"""拼接接不上时，预览面板顶部写出原因和办法；下一帧接上后提示消失。"""

import random

import pytest
from PySide6.QtGui import QImage

from tests.long_capture_sim import PAGE_W, VIEW_H, open_window, simulate, synthetic_page


@pytest.fixture
def window(qapp, monkeypatch):
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
