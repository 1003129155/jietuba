"""长截图窗口和预览面板都去掉系统圆角和阴影：两者紧贴截图区，系统画的会被截进每一帧。"""

import pytest

from tests.long_capture_sim import open_window


@pytest.fixture
def window(qapp, monkeypatch):
    win = open_window(monkeypatch)
    yield win
    if win._stitcher is not None:
        win._cleanup()


def test_window_and_preview_panel_drop_system_corners(monkeypatch, window):
    import core.platform_utils as platform_utils
    calls = []
    monkeypatch.setattr(platform_utils, "set_window_rounded_corners",
                        lambda hwnd, rounded: calls.append((hwnd, rounded)) or True)
    window._drop_system_corners()
    assert (int(window.winId()), False) in calls
    assert (int(window.preview_panel.winId()), False) in calls
