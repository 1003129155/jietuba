# -*- coding: utf-8 -*-
"""循环垃圾只在主线程回收：后台线程分配再多也不触发回收，主线程按阈值逐级回收。"""

import gc
import threading

import pytest

from core.main_thread_gc import MainThreadGC


@pytest.fixture
def collections(qapp):
    """记录每次回收发生在哪个线程、回收的是哪一级。"""
    seen = []

    def record(phase, info):
        if phase == "start":
            seen.append((threading.current_thread() is threading.main_thread(), info["generation"]))

    gc.collect()
    gc.callbacks.append(record)
    guard = MainThreadGC()
    yield guard, seen
    guard.uninstall()
    gc.callbacks.remove(record)
    assert gc.isenabled()


def make_cycles(count):
    for _ in range(count):
        node = []
        node.append(node)


def test_background_threads_never_trigger_a_collection(collections):
    guard, seen = collections
    worker = threading.Thread(target=make_cycles, args=(50_000,))
    worker.start()
    worker.join()

    assert seen == []
    guard.check()
    assert seen and all(on_main for on_main, _generation in seen)


def test_check_follows_the_automatic_thresholds(collections):
    guard, seen = collections
    threshold0, threshold1, _threshold2 = gc.get_threshold()

    guard.check()
    assert seen == []  # 新分配还没超过第 0 级阈值

    # 第 1 级的计数要超过阈值才回收第 1 级，所以先有 threshold1 + 1 次第 0 级
    for _ in range(threshold1 + 2):
        make_cycles(threshold0 * 2)
        guard.check()
    assert [generation for _on_main, generation in seen] == [0] * (threshold1 + 1) + [1]


def test_uninstall_hands_collection_back(qapp):
    guard = MainThreadGC()
    assert not gc.isenabled()
    guard.uninstall()
    assert gc.isenabled()
