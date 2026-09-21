# -*- coding: utf-8 -*-
"""进程内"上次截图区域"记忆（core/last_capture_region.py）"""
import pytest
from PySide6.QtCore import QRect

from core import last_capture_region as region_module
from core.last_capture_region import get_last_region, set_last_region


@pytest.fixture(autouse=True)
def _reset_region():
    """模块级单例，测试之间必须互相隔离。"""
    region_module._last_region = None
    yield
    region_module._last_region = None


def test_get_before_any_set_is_none():
    assert get_last_region() is None


def test_set_then_get_round_trips():
    set_last_region(QRect(10, 20, 300, 200))
    assert get_last_region() == QRect(10, 20, 300, 200)


def test_only_the_latest_call_is_kept():
    set_last_region(QRect(0, 0, 100, 100))
    set_last_region(QRect(500, 500, 50, 50))
    assert get_last_region() == QRect(500, 500, 50, 50)


def test_set_stores_a_copy_not_a_reference():
    original = QRect(0, 0, 100, 100)
    set_last_region(original)
    original.setWidth(999)
    assert get_last_region().width() == 100


def test_get_returns_a_copy_not_the_stored_instance():
    set_last_region(QRect(0, 0, 100, 100))
    got = get_last_region()
    got.setWidth(999)
    assert get_last_region().width() == 100
