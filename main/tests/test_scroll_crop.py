"""长截图的裁剪：往回滚到想截断的地方，从工具栏把长图裁到当前画面。

用合成页面模拟滚动和截图，走工具栏的信号到导出，检查导出的长图正好在当前画面处截断。
"""

import pytest

from stitch import auto_scroll
from tests.long_capture_sim import PAGE_W, VIEW_H, open_window, simulate, synthetic_page


@pytest.fixture
def window(qapp, monkeypatch):
    win = open_window(monkeypatch)
    yield win
    if win._stitcher is not None:
        win._cleanup()


def _scroll_down_then_back(monkeypatch, window):
    """从页面开头往下滚 10 步（每步 120 像素），再往回滚 3 步，返回模拟页面。"""
    sim = simulate(monkeypatch, window, synthetic_page(2600, seed=9), 0)
    window._do_capture()
    for _ in range(10):
        sim.scroll_and_capture(-3)
    for _ in range(3):
        sim.scroll_and_capture(3)
    assert sim.top == 840
    return sim


def test_crop_bottom_ends_the_capture_at_the_current_view(qtbot, monkeypatch, window):
    sim = _scroll_down_then_back(monkeypatch, window)
    window.toolbar.crop_requested.emit("bottom")
    window._on_finish()
    assert window.captured["image"].convert("RGB").tobytes() == sim.page.crop((0, 0, PAGE_W, 840 + VIEW_H)).tobytes()


def test_crop_top_starts_the_capture_at_the_current_view(qtbot, monkeypatch, window):
    sim = _scroll_down_then_back(monkeypatch, window)
    window.toolbar.crop_requested.emit("top")
    window._on_finish()
    assert window.captured["image"].convert("RGB").tobytes() == sim.page.crop((0, 840, PAGE_W, 1200 + VIEW_H)).tobytes()


def test_preview_and_box_follow_the_crop(qtbot, monkeypatch, window):
    _scroll_down_then_back(monkeypatch, window)
    # 后台逐帧回传，慢机器上要等最深的一帧拼完才是最终尺寸；裁剪本身排在已提交的帧之后
    qtbot.waitUntil(lambda: window.toolbar.size_label.text() == f"{PAGE_W} × {1200 + VIEW_H}")
    window._crop("top")
    qtbot.waitUntil(lambda: window._latest_box[0] == 0)
    preview = window._latest_preview
    assert preview.height() == round((1200 + VIEW_H - 840) * preview.width() / PAGE_W)
    assert window.toolbar.size_label.text() == f"{PAGE_W} × {1200 + VIEW_H - 840}"


def test_crop_during_auto_scroll_cuts_at_the_view_on_screen(qtbot, monkeypatch, window):
    """自动滚动刚注入一步、还没截图时裁剪：先截下这一步，裁剪才对准屏幕上停住的画面。"""
    monkeypatch.setattr(auto_scroll, "SETTLE_MS", 10_000)
    sim = simulate(monkeypatch, window, synthetic_page(2600, seed=9), 0)
    window._do_capture()
    sim.scroll_and_capture(-3)
    window._toggle_auto_scroll()
    top = sim.top
    assert top > 120
    window._crop("top")
    assert not window._auto_scroller.running
    window._on_finish()
    assert window.captured["image"].convert("RGB").tobytes() == sim.page.crop((0, top, PAGE_W, top + VIEW_H)).tobytes()


def test_crop_right_after_scrolling_takes_the_pending_frame_first(qtbot, monkeypatch, window):
    """刚滚完、截图还在等冷却时就裁剪：先截下当前画面，裁剪才对准它。"""
    sim = _scroll_down_then_back(monkeypatch, window)
    sim.inject(3 * 120)  # 往上滚 3 格，不截图
    assert sim.top == 720 and window.capture_timer.isActive()
    window._crop("top")
    window._on_finish()
    assert window.captured["image"].convert("RGB").tobytes() == sim.page.crop((0, 720, PAGE_W, 1200 + VIEW_H)).tobytes()


def test_capturing_after_a_crop_keeps_growing(qtbot, monkeypatch, window):
    """裁掉底部后再往下滚，长图从当前画面接着长，不会把裁掉的部分找回来再拼一遍。"""
    sim = _scroll_down_then_back(monkeypatch, window)
    window._crop("bottom")
    for _ in range(5):
        sim.scroll_and_capture(-3)
    window._on_finish()
    assert window.captured["image"].convert("RGB").tobytes() == sim.page.crop((0, 0, PAGE_W, 1440 + VIEW_H)).tobytes()
