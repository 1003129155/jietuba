# -*- coding: utf-8 -*-
"""自由笔迹的平滑：增量改写末尾几段的结果必须和整条重算一致，首末点和直线端点不动，
稀疏的点原样穿过，密集的台阶被抹平。"""
import math

import pytest
from PySide6.QtCore import QPointF
from PySide6.QtGui import QPainterPath

from tools.base import FreehandStroke, Tool, _catmull_rom, _relax


def _feed(points, smooth=True):
    stroke = FreehandStroke(QPointF(*points[0]), 45, smooth=smooth)
    for x, y in points[1:]:
        stroke.extend(QPointF(x, y), straight=False)
    return stroke


def _elements(path: QPainterPath):
    return [(path.elementAt(i).x, path.elementAt(i).y) for i in range(path.elementCount())]


def _accepted(raw):
    """笔迹实际收下的点：和 FreehandStroke 一样先过 Tool.should_append_point。"""
    kept = [QPointF(*raw[0])]
    for x, y in raw[1:]:
        if Tool.should_append_point(kept[-1], QPointF(x, y)):
            kept.append(QPointF(x, y))
    return [(p.x(), p.y()) for p in kept]


def _reference(raw):
    """整条重算一遍：两遍按间距平滑，再逐段 Catmull-Rom。"""
    raw = _accepted(raw)
    def relax_all(points):
        return [points[0]] + [_relax(points[i - 1], points[i], points[i + 1])
                              for i in range(1, len(points) - 1)] + [points[-1]]
    twice = relax_all(relax_all(raw))
    n = len(twice) - 1
    out = [twice[0]]
    for i in range(n):
        c1, c2 = _catmull_rom(twice[max(i - 1, 0)], twice[i], twice[i + 1], twice[min(i + 2, n)])
        out += [c1, c2, twice[i + 1]]
    return out


def _wobbly_stroke():
    """一段慢（1～2px 间距、取整）接一段快（20px 以上间距）。"""
    points = []
    for i in range(80):
        t = i / 79 * math.pi
        points.append((round(40 + i * 1.6), round(100 + 18 * math.sin(t))))
    x, y = points[-1]
    for i in range(1, 15):
        points.append((x + 22 * i, y + 30 * math.sin(i)))
    return points


def test_incremental_update_matches_a_full_recompute():
    raw = _wobbly_stroke()
    got = _elements(_feed(raw).path)
    want = _reference(raw)
    assert len(got) == len(want)
    for a, b in zip(got, want):
        assert a == pytest.approx(b, abs=1e-9)


def test_every_prefix_matches_a_full_recompute():
    """画到一半的样子也要对：末尾几段每来一个点都在变。"""
    raw = _wobbly_stroke()[:30]
    stroke = FreehandStroke(QPointF(*raw[0]), 45, smooth=True)
    for k in range(1, len(raw)):
        stroke.extend(QPointF(*raw[k]), straight=False)
        want = _reference(raw[:k + 1])
        got = _elements(stroke.path)
        assert got == pytest.approx(want, abs=1e-9), f"第 {k} 个点之后不一致"


def test_start_and_end_stay_on_the_samples():
    raw = _wobbly_stroke()
    path = _feed(raw).path
    assert _elements(path)[0] == raw[0]
    assert _elements(path)[-1] == pytest.approx(raw[-1])


def test_sparse_samples_are_passed_through():
    """快画时点就是形状：间距够大的点不挪，曲线逐个穿过。"""
    raw = [(200 + 60 * math.cos(a / 10), 200 + 60 * math.sin(a / 10)) for a in range(0, 60, 2)]
    ends = _elements(_feed(raw).path)[3::3]
    assert ends == pytest.approx(raw[1:])


def test_dense_staircase_is_flattened():
    """慢画一条缓坡，整数坐标是一级级台阶；平滑后离理想直线更近。"""
    ideal = lambda x: 50 + (x - 20) * 0.09  # noqa: E731
    raw = [(20 + i * 2, round(ideal(20 + i * 2))) for i in range(150)]
    polyline = max(abs(y - ideal(x)) for x, y in raw)
    curve = _feed(raw).path
    smoothed = max(abs(curve.pointAtPercent(t / 2000).y() - ideal(curve.pointAtPercent(t / 2000).x()))
                   for t in range(100, 1900))
    assert smoothed < polyline * 0.6


def test_shift_line_keeps_its_ends_and_stays_straight():
    stroke = FreehandStroke(QPointF(0, 0), 90, smooth=True)
    for i in range(1, 20):
        stroke.extend(QPointF(i * 2, round(math.sin(i / 3) * 3)), straight=False)
    anchor = stroke.last_point
    line_from = stroke.path.elementCount() - 1
    stroke.extend(QPointF(120, anchor.y() + 4), straight=True)
    stroke.extend(QPointF(140, anchor.y() + 5), straight=True)
    for i in range(1, 20):
        stroke.extend(QPointF(140 + i * 2, anchor.y() + i), straight=False)

    elements = _elements(stroke.path)
    assert elements[line_from] == (anchor.x(), anchor.y())
    assert elements[line_from + 1] == (140, anchor.y())
    assert stroke.path.elementAt(line_from + 1).isLineTo()


def test_smoothing_off_is_a_polyline():
    raw = _wobbly_stroke()
    path = _feed(raw, smooth=False).path
    assert all(path.elementAt(i).isLineTo() for i in range(1, path.elementCount()))
