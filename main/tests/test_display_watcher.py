# -*- coding: utf-8 -*-
"""显示器配置变化监听：一次变化连发的几条 WM_DISPLAYCHANGE 合并成一次回调。"""
import ctypes
from ctypes import wintypes
from unittest.mock import Mock

import pytest

from capture.display_watcher import WM_DISPLAYCHANGE, DisplayChangeWatcher


def _send(watcher, message):
    msg = wintypes.MSG()
    msg.message = message
    return watcher._filter.nativeEventFilter(b"windows_generic_MSG", ctypes.addressof(msg))


@pytest.fixture
def watcher(qapp):
    on_change = Mock()
    watcher = DisplayChangeWatcher(on_change, settle_ms=30)
    yield watcher, on_change
    watcher.close()


def test_bursts_of_display_changes_call_back_once(watcher, qtbot):
    watcher, on_change = watcher
    for _ in range(3):
        assert _send(watcher, WM_DISPLAYCHANGE) == (False, 0), "消息要继续交给 Qt 处理"
    qtbot.waitUntil(lambda: on_change.called)
    qtbot.wait(80)
    on_change.assert_called_once()


def test_other_messages_are_ignored(watcher, qtbot):
    watcher, on_change = watcher
    _send(watcher, 0x0312)  # WM_HOTKEY
    qtbot.wait(80)
    on_change.assert_not_called()


def test_close_uninstalls_the_filter(qapp, monkeypatch):
    watcher = DisplayChangeWatcher(Mock(), settle_ms=10)
    remove = Mock()
    monkeypatch.setattr(qapp, "removeNativeEventFilter", remove)
    watcher.close()
    remove.assert_called_once_with(watcher._filter)
    type(qapp).removeNativeEventFilter(qapp, watcher._filter)
