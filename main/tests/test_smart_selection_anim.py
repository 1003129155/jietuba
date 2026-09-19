# -*- coding: utf-8 -*-
"""
智能选区补间测试

补间本身很短，肉眼分不出对错，但它挂在 hover 这条每次鼠标移动都会触发的路径上，
几个边界条件错了就会表现为"选区慢慢爬"或"截到半截矩形"，而这些都不好复现。
这里直接驱动 SmartSelectionAnimator，用手喂时钟，验证目标重复、中途改目标、
按下打断这三种情况。
"""
import time

import pytest
from PySide6.QtCore import QRectF

from canvas.smart_selection_anim import SmartSelectionAnimator


WIN_A = QRectF(0, 0, 400, 300)
WIN_B = QRectF(900, 500, 600, 400)


@pytest.fixture
def anim(qapp):
    applied = []
    a = SmartSelectionAnimator(applied.append)
    a.applied = applied
    yield a
    a.stop()


def test_empty_start_snaps(anim):
    anim.animate_to(QRectF(), WIN_A)
    assert anim.applied == [WIN_A]
    assert not anim.is_running


def test_tiny_move_snaps(anim):
    nudged = WIN_A.adjusted(1, 1, 1, 1)
    anim.animate_to(WIN_A, nudged)
    assert anim.applied == [nudged]
    assert not anim.is_running


def test_cross_window_jump_animates(anim):
    anim.animate_to(WIN_A, WIN_B)
    assert anim.is_running
    assert anim.applied == []


def test_repeated_same_target_keeps_one_run(anim):
    """同一个窗口内 hover 会反复送同一个目标，不能每次都重置计时。"""
    anim.animate_to(WIN_A, WIN_B)
    anim._elapsed.restart()
    for _ in range(5):
        anim.animate_to(anim.target, WIN_B)
    anim._tick()
    assert anim.is_running
    assert anim.applied, "补间应当在推进，而不是被反复重置到原地"


def test_retarget_restarts_from_current(anim):
    """途中换窗口从当前插值位置起步，不排队播放上一段。"""
    anim.animate_to(WIN_A, WIN_B)
    mid = QRectF(400, 200, 500, 350)
    win_c = QRectF(1600, 100, 300, 300)
    anim.animate_to(mid, win_c)
    assert anim.target == win_c
    assert anim._from == mid


def test_snap_to_interrupts(anim):
    """按下时必须立刻落在真实窗口矩形上，并停掉补间。"""
    anim.animate_to(WIN_A, WIN_B)
    anim.snap_to(WIN_B)
    assert not anim.is_running
    assert anim.applied[-1] == WIN_B


def test_empty_target_stops(anim):
    anim.animate_to(WIN_A, WIN_B)
    anim.animate_to(WIN_A, QRectF())
    assert not anim.is_running


def test_tick_past_duration_lands_exactly(anim):
    """最后一帧必须是目标矩形本身，不能停在插值精度上。"""
    anim.DURATION_MS = 1
    anim.animate_to(WIN_A, WIN_B)
    time.sleep(0.01)  # 越过终点
    anim._tick()
    assert anim.applied[-1] == WIN_B
    assert not anim.is_running
