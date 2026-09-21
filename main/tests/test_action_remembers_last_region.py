# -*- coding: utf-8 -*-
"""ActionTools._remember_last_region —— 把本地选区坐标换算成虚拟桌面绝对坐标存起来。

只测这一个纯函数：它是"恢复上次选区"快捷键的写入侧，换算错了坐标，
快捷键就会把选区放到错误的位置，且没有任何报错能提示出来。
"""
from types import SimpleNamespace

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


def _make_action_tools(virtual_x=0, virtual_y=0):
    tools = ActionTools.__new__(ActionTools)
    tools.parent_window = SimpleNamespace(virtual_x=virtual_x, virtual_y=virtual_y)
    return tools


def test_local_rect_is_offset_by_the_virtual_desktop_origin():
    tools = _make_action_tools(virtual_x=100, virtual_y=50)
    tools._remember_last_region(QRectF(10, 20, 300, 200))
    assert get_last_region().getRect() == (110, 70, 300, 200)


def test_negative_virtual_origin_is_handled_as_well():
    """次屏在主屏左侧/上方时，虚拟桌面原点可以是负数。"""
    tools = _make_action_tools(virtual_x=-1920, virtual_y=0)
    tools._remember_last_region(QRectF(50, 50, 400, 300))
    assert get_last_region().getRect() == (-1870, 50, 400, 300)


def test_empty_rect_does_not_overwrite_the_remembered_region():
    tools = _make_action_tools()
    tools._remember_last_region(QRectF(10, 20, 300, 200))
    tools._remember_last_region(QRectF())
    assert get_last_region().getRect() == (10, 20, 300, 200)


def test_missing_parent_window_does_not_raise():
    tools = ActionTools.__new__(ActionTools)
    tools.parent_window = None
    tools._remember_last_region(QRectF(10, 20, 300, 200))
    assert get_last_region() is None
