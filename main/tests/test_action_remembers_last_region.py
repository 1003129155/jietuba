# -*- coding: utf-8 -*-
"""ActionTools._remember_last_region —— 把选区的场景坐标存成"上次选区"。

场景坐标就是虚拟桌面绝对坐标：CanvasScene 直接拿虚拟桌面矩形当 sceneRect，
所以这里不做任何换算。存错了坐标，"恢复上次选区"快捷键就会把选区放到错误的
位置，且没有任何报错能提示出来。
"""
import pytest
from PySide6.QtCore import QRectF

from core import last_capture_region as region_module
from core.last_capture_region import get_last_region
from tools.action import ActionTools


@pytest.fixture(autouse=True)
def _reset_region():
    region_module._last_region = None
    yield
    region_module._last_region = None


def _make_action_tools():
    return ActionTools.__new__(ActionTools)


def test_scene_rect_is_stored_without_any_conversion():
    tools = _make_action_tools()
    tools._remember_last_region(QRectF(10, 20, 300, 200))
    assert get_last_region().getRect() == (10, 20, 300, 200)


def test_region_on_a_monitor_left_of_the_primary_keeps_its_negative_coordinates():
    """副屏在主屏左侧时场景坐标是负的，不能被换算掉或当成越界丢弃。"""
    tools = _make_action_tools()
    tools._remember_last_region(QRectF(-1870, 50, 400, 300))
    assert get_last_region().getRect() == (-1870, 50, 400, 300)


def test_empty_rect_does_not_overwrite_the_remembered_region():
    tools = _make_action_tools()
    tools._remember_last_region(QRectF(10, 20, 300, 200))
    tools._remember_last_region(QRectF())
    assert get_last_region().getRect() == (10, 20, 300, 200)
