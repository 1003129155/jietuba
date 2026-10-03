"""长截图预览面板：短边固定、按原比例显示，放不下时视口跟着最新一帧的框走。"""

import pytest
from PySide6.QtGui import QColor, QImage

from stitch.scroll_window import PreviewPanel

LIMIT = 600


@pytest.fixture
def panel(qapp, monkeypatch):
    monkeypatch.setattr(PreviewPanel, "_max_length", lambda self: LIMIT)
    panel = PreviewPanel()
    yield panel
    panel.close()
    panel.deleteLater()


def _strip(panel, length, vertical=True):
    side = panel._fixed_side
    image = QImage(side, length, QImage.Format.Format_RGB32) if vertical \
        else QImage(length, side, QImage.Format.Format_RGB32)
    image.fill(QColor(200, 210, 220))
    return image


def test_short_strip_is_shown_whole(panel):
    panel.update_preview(_strip(panel, 400), "vertical", (300, 400))
    assert (panel.width(), panel.height()) == (panel._fixed_side, 400)
    assert panel._offset == 0


def test_long_strip_shows_the_latest_frame_at_the_end(panel):
    panel.update_preview(_strip(panel, 3000), "vertical", (2860, 3000))
    assert panel.height() == LIMIT
    assert panel._offset == 3000 - LIMIT


def test_viewport_stays_put_while_the_frame_is_visible(panel):
    image = _strip(panel, 3000)
    panel.update_preview(image, "vertical", (2860, 3000))
    panel.update_preview(image, "vertical", (1000, 1140))  # 回滚到长图中段
    offset = panel._offset
    for box in [(1100, 1240), (1300, 1440), (1050, 1190)]:  # 框在视口里来回移动
        panel.update_preview(image, "vertical", box)
        assert panel._offset == offset


def test_viewport_moves_just_enough_when_the_frame_leaves_it(panel):
    image = _strip(panel, 3000)
    panel.update_preview(image, "vertical", (2860, 3000))
    panel.update_preview(image, "vertical", (1000, 1140))  # 回滚到视口之外
    margin = panel._offset - 1000
    assert 0 < -margin <= 40
    assert panel._offset <= 1000 < 1140 <= panel._offset + LIMIT
    panel.update_preview(image, "vertical", (0, 140))
    assert panel._offset == 0


def test_horizontal_strip_runs_along_the_width(panel):
    panel.update_preview(_strip(panel, 3000, vertical=False), "horizontal", (2860, 3000))
    assert (panel.width(), panel.height()) == (LIMIT, panel._fixed_side)
    assert panel._offset == 3000 - LIMIT


@pytest.mark.parametrize("box", [(0, 140), (1500, 1640), (2860, 3000)])
def test_painting_a_clipped_strip_works(panel, box):
    panel.update_preview(_strip(panel, 3000), "vertical", box)
    assert not panel.grab().isNull()


def test_missing_image_falls_back_to_the_placeholder(panel):
    panel.update_preview(_strip(panel, 400), "vertical", (300, 400))
    panel.update_preview(None, "vertical")
    assert panel._image is None and panel._box is None
    assert not panel.grab().isNull()


def test_warning_shows_the_reason_until_cleared(panel):
    panel.update_preview(_strip(panel, 400), "vertical", (300, 400))
    panel.show_warning("Couldn't join. Scroll back")
    label = panel.warning_label
    assert not label.isHidden() and label.text() == "Couldn't join. Scroll back"
    assert label.x() + label.width() <= panel.width()
    panel.clear_warning()
    assert label.isHidden()
