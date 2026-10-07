# -*- coding: utf-8 -*-
"""文字识别结果窗口的摆放：选屏、五种位置、放不下时的退回。纯几何，不开窗口。"""
from types import SimpleNamespace

import pytest
from PySide6.QtCore import QPoint, QRect, QSize

from text_recognition.placement import CURSOR_OFFSET, GAP, pick_screen, window_position

SIZE = QSize(520, 420)


def _screen(x, y, width, height, taskbar=40):
    rect = QRect(x, y, width, height)
    available = rect.adjusted(0, 0, 0, -taskbar)
    return SimpleNamespace(geometry=lambda: rect, availableGeometry=lambda: available)


MAIN = _screen(0, 0, 1920, 1080)
SIDE = _screen(1920, 0, 2560, 1440)
SCREENS = [MAIN, SIDE]
AVAILABLE = MAIN.availableGeometry()
ON_MAIN = QRect(300, 200, 400, 300)
ON_SIDE = QRect(2100, 200, 400, 300)


def _pick(position, anchor=None, cursor=QPoint(100, 100), last_center=None):
    return pick_screen(position, anchor, cursor, last_center, SCREENS, MAIN)


class TestPickScreen:

    def test_region_screen_follows_the_capture_area_not_the_cursor(self):
        assert _pick("region_screen", ON_SIDE) is SIDE

    def test_without_a_region_it_follows_the_cursor(self):
        assert _pick("region_screen", None, QPoint(2000, 100)) is SIDE

    def test_a_region_off_every_screen_falls_back_to_the_cursor_then_the_primary(self):
        off = QRect(9000, 9000, 10, 10)
        assert _pick("region_screen", off, QPoint(2000, 100)) is SIDE
        assert _pick("region_screen", off, QPoint(9000, 9000)) is MAIN

    def test_primary_screen_ignores_everything_else(self):
        assert _pick("primary_screen", ON_SIDE, QPoint(2000, 100), QPoint(2100, 100)) is MAIN

    def test_beside_region_uses_the_screen_of_the_region(self):
        assert _pick("beside_region", ON_SIDE) is SIDE

    def test_near_cursor_follows_the_cursor_not_the_region(self):
        assert _pick("near_cursor", ON_SIDE, QPoint(100, 100)) is MAIN

    def test_near_cursor_off_every_screen_uses_the_region_screen(self):
        assert _pick("near_cursor", ON_SIDE, QPoint(9000, 9000)) is SIDE

    def test_last_position_follows_the_saved_center_not_the_region(self):
        assert _pick("last_position", ON_MAIN, last_center=QPoint(2100, 100)) is SIDE

    @pytest.mark.parametrize("last_center", [None, QPoint(9000, 9000)])
    def test_last_position_without_a_usable_record_acts_like_the_default(self, last_center):
        assert _pick("last_position", ON_SIDE, QPoint(100, 100), last_center) is SIDE


def _rect(position, screen=MAIN, **kwargs):
    return QRect(window_position(position, SIZE, screen, **kwargs), SIZE)


@pytest.mark.parametrize("position", ["region_screen", "primary_screen"])
def test_center_positions_ignore_the_region(position):
    assert _rect(position, anchor=QRect(50, 50, 200, 100)).center() == AVAILABLE.center()


def test_the_center_is_the_center_of_the_available_area_not_of_the_screen():
    assert _rect("region_screen").center() == AVAILABLE.center() != MAIN.geometry().center()


class TestBesideRegion:

    def _at(self, anchor):
        return window_position("beside_region", SIZE, MAIN, anchor=anchor)

    def test_without_a_region_it_centers(self):
        assert _rect("beside_region").center() == AVAILABLE.center()

    def test_prefers_the_right_side_aligned_to_the_top(self):
        anchor = QRect(100, 200, 300, 200)
        assert self._at(anchor) == QPoint(anchor.right() + 1 + GAP, 200)

    def test_moves_to_the_left_when_the_right_has_no_room(self):
        anchor = QRect(1500, 200, 300, 200)
        assert self._at(anchor) == QPoint(1500 - GAP - SIZE.width(), 200)

    def test_goes_below_when_neither_side_has_room(self):
        anchor = QRect(100, 100, 1700, 300)
        assert self._at(anchor) == QPoint(100, anchor.bottom() + 1 + GAP)

    def test_goes_above_when_only_the_top_has_room(self):
        anchor = QRect(100, 700, 1700, 300)
        assert self._at(anchor) == QPoint(100, anchor.top() - GAP - SIZE.height())

    def test_falls_back_to_the_center_when_nothing_fits(self):
        assert _rect("beside_region", anchor=QRect(10, 10, 1900, 1020)).center() == AVAILABLE.center()

    def test_top_alignment_is_clamped_to_the_bottom_of_the_screen(self):
        anchor = QRect(100, 900, 300, 130)
        assert self._at(anchor) == QPoint(anchor.right() + 1 + GAP, AVAILABLE.bottom() + 1 - SIZE.height())

    def test_left_alignment_is_clamped_to_the_right_of_the_screen(self):
        # 左右都放不下时改放下面，横向与区域左边对齐，但不能伸出屏幕右边
        anchor = QRect(900, 100, 1000, 300)
        pos = window_position("beside_region", QSize(1200, 420), MAIN, anchor=anchor)
        assert pos == QPoint(AVAILABLE.right() + 1 - 1200, anchor.bottom() + 1 + GAP)

    @pytest.mark.parametrize("anchor", [
        QRect(0, 0, 100, 100), QRect(1820, 0, 100, 100), QRect(0, 940, 100, 100),
        QRect(1820, 940, 100, 100), QRect(900, 500, 100, 100), QRect(0, 0, 1920, 200),
        QRect(0, 840, 1920, 200), QRect(0, 0, 200, 1040), QRect(1720, 0, 200, 1040),
        QRect(300, 100, 1300, 800), QRect(0, 0, 1920, 1040),
    ])
    def test_stays_on_screen_and_never_covers_the_region(self, anchor):
        rect = QRect(self._at(anchor), SIZE)
        assert AVAILABLE.contains(rect)
        # 四周都放不下时才退回正中，那时允许盖住区域
        assert rect.center() == AVAILABLE.center() or not rect.intersects(anchor)


class TestNearCursor:

    def _at(self, cursor, screen=MAIN):
        return window_position("near_cursor", SIZE, screen, cursor=cursor)

    def test_sits_at_the_lower_right_of_the_cursor(self):
        assert self._at(QPoint(300, 200)) == QPoint(300 + CURSOR_OFFSET, 200 + CURSOR_OFFSET)

    def test_flips_to_the_left_near_the_right_edge(self):
        assert self._at(QPoint(1800, 200)) == QPoint(1800 - CURSOR_OFFSET - SIZE.width(), 200 + CURSOR_OFFSET)

    def test_flips_upward_near_the_bottom_edge(self):
        assert self._at(QPoint(300, 900)) == QPoint(300 + CURSOR_OFFSET, 900 - CURSOR_OFFSET - SIZE.height())

    def test_flips_both_ways_in_the_corner(self):
        assert self._at(QPoint(1900, 1000)) == QPoint(
            1900 - CURSOR_OFFSET - SIZE.width(), 1000 - CURSOR_OFFSET - SIZE.height())

    def test_never_leaves_the_screen_even_on_a_narrow_one(self):
        narrow = _screen(0, 0, 400, 300, taskbar=0)
        rect = QRect(self._at(QPoint(200, 150), narrow), SIZE)
        assert (rect.left(), rect.top()) == (0, 0)

    def test_a_cursor_that_is_not_on_the_screen_centers(self):
        assert _rect("near_cursor", cursor=QPoint(2100, 100)).center() == AVAILABLE.center()

    def test_without_a_cursor_it_centers(self):
        assert _rect("near_cursor").center() == AVAILABLE.center()


class TestLastPosition:

    def test_the_window_comes_back_around_the_saved_center(self):
        assert _rect("last_position", last_center=QPoint(900, 500)).center() == QPoint(900, 500)

    def test_a_saved_center_near_the_corners_is_pulled_back_on_screen(self):
        assert window_position("last_position", SIZE, MAIN, last_center=QPoint(10, 10)) == QPoint(0, 0)
        assert window_position("last_position", SIZE, MAIN, last_center=QPoint(1915, 1035)) == QPoint(
            AVAILABLE.right() + 1 - SIZE.width(), AVAILABLE.bottom() + 1 - SIZE.height())

    def test_a_saved_center_on_another_screen_centers_on_this_one(self):
        assert _rect("last_position", last_center=QPoint(2100, 100)).center() == AVAILABLE.center()

    def test_without_a_record_it_centers(self):
        assert _rect("last_position").center() == AVAILABLE.center()

    def test_the_saved_center_works_on_a_screen_that_is_not_at_the_origin(self):
        center = QPoint(3000, 700)
        rect = QRect(window_position("last_position", SIZE, SIDE, last_center=center), SIZE)
        assert rect.center() == center


# ── 多显示器布局：副屏在主屏左边/上方时坐标是负的，竖屏、不同分辨率混用 ──
LEFT = _screen(-1920, 0, 1920, 1080)
ABOVE = _screen(0, -1080, 1920, 1080)
PORTRAIT = _screen(-3000, -420, 1080, 1920)
LAYOUT = [MAIN, SIDE, LEFT, ABOVE, PORTRAIT]


def _near_center(rect, point):
    # 负坐标下 QRect.center() 向零取整，往返可能差 1 像素
    return abs(rect.center().x() - point.x()) <= 1 and abs(rect.center().y() - point.y()) <= 1


def _sample_anchor(screen):
    geometry = screen.geometry()
    return QRect(geometry.left() + 100, geometry.top() + 100, 300, 200)


@pytest.mark.parametrize("screen", LAYOUT, ids=["main", "side", "left", "above", "portrait"])
@pytest.mark.parametrize("position", ["region_screen", "beside_region", "near_cursor", "last_position"])
def test_every_position_lands_inside_the_screen_it_was_picked_for(screen, position):
    anchor = _sample_anchor(screen)
    point = anchor.center()
    picked = pick_screen(position, anchor, point, point, LAYOUT, MAIN)
    assert picked is screen
    rect = QRect(window_position(position, SIZE, picked, anchor, point, point), SIZE)
    assert picked.availableGeometry().contains(rect)


@pytest.mark.parametrize("screen", LAYOUT, ids=["main", "side", "left", "above", "portrait"])
def test_primary_screen_is_chosen_whatever_the_layout(screen):
    anchor = _sample_anchor(screen)
    picked = pick_screen("primary_screen", anchor, anchor.center(), anchor.center(), LAYOUT, MAIN)
    rect = QRect(window_position("primary_screen", SIZE, picked, anchor), SIZE)
    assert picked is MAIN
    assert _near_center(rect, MAIN.availableGeometry().center())


def test_the_default_follows_the_region_to_a_screen_left_of_the_primary():
    anchor = _sample_anchor(LEFT)
    picked = pick_screen("region_screen", anchor, QPoint(100, 100), None, LAYOUT, MAIN)
    rect = QRect(window_position("region_screen", SIZE, picked, anchor), SIZE)
    assert picked is LEFT
    assert _near_center(rect, LEFT.availableGeometry().center())


def test_beside_region_on_a_negative_screen_keeps_the_same_gap():
    anchor = QRect(-1800, 200, 300, 200)
    assert window_position("beside_region", SIZE, LEFT, anchor=anchor) == QPoint(anchor.right() + 1 + GAP, 200)


def test_near_cursor_on_a_negative_screen_flips_at_that_screens_edge():
    cursor = QPoint(-500, 300)
    assert window_position("near_cursor", SIZE, LEFT, cursor=cursor) == QPoint(
        cursor.x() - CURSOR_OFFSET - SIZE.width(), cursor.y() + CURSOR_OFFSET)


def test_a_saved_center_comes_back_on_a_screen_above_the_primary():
    center = QPoint(900, -500)
    rect = QRect(window_position("last_position", SIZE, ABOVE, last_center=center), SIZE)
    assert _near_center(rect, center)
