# -*- coding: utf-8 -*-
"""前台窗口跟踪器：只认能收到模拟按键的外部窗口，取样不合格时保留上一次的目标。

这里每条断言都对应一种会把 Ctrl+V 发错地方的情况——目标被改成拾取窗口自己、
被改成任务栏、被改成收不到按键的浮窗，或者指向一个已经关掉的窗口。
"""

import pytest

from clipboard.controllers import foreground_tracker as tracker_module
from clipboard.controllers.foreground_tracker import ForegroundWindowTracker

PICKER_HWND = 99


@pytest.fixture
def fake_desktop(monkeypatch):
    """用一张 hwnd → (类名, 能否取得焦点) 的表冒充系统，不依赖真实窗口。"""
    windows = {}
    state = {"foreground": None}

    monkeypatch.setattr(tracker_module, "get_foreground_hwnd", lambda: state["foreground"])
    monkeypatch.setattr(tracker_module, "is_alive", lambda hwnd: hwnd in windows)
    monkeypatch.setattr(tracker_module, "get_window_class", lambda hwnd: windows[hwnd][0])
    monkeypatch.setattr(tracker_module, "can_take_focus", lambda hwnd: windows[hwnd][1])

    def add(hwnd, cls="Notepad", focusable=True):
        windows[hwnd] = (cls, focusable)
        return hwnd

    def focus(hwnd):
        state["foreground"] = hwnd

    def destroy(hwnd):
        windows.pop(hwnd, None)

    return type("FakeDesktop", (), {
        "add": staticmethod(add),
        "focus": staticmethod(focus),
        "destroy": staticmethod(destroy),
    })


@pytest.fixture
def tracker(fake_desktop):
    """拾取窗口自己是唯一被排除的窗口。"""
    instance = ForegroundWindowTracker()
    instance.set_excluded(lambda hwnd: hwnd == PICKER_HWND)
    fake_desktop.add(PICKER_HWND, cls="Qt5152QWindowIcon")
    return instance


def test_external_window_becomes_target(fake_desktop, tracker):
    fake_desktop.focus(fake_desktop.add(11))

    assert tracker.sample() is True
    assert tracker.target_hwnd == 11


def test_picker_window_does_not_overwrite_target(fake_desktop, tracker):
    """点回拾取窗口时前台就是它，覆盖目标会让 Ctrl+V 落进搜索框。"""
    fake_desktop.focus(fake_desktop.add(11))
    tracker.sample()

    fake_desktop.focus(PICKER_HWND)

    assert tracker.sample() is False
    assert tracker.target_hwnd == 11


def test_sibling_app_window_is_a_valid_target(fake_desktop, tracker):
    """内容编辑窗口和拾取窗口同属一个进程，但用户确实会往里粘。"""
    fake_desktop.focus(fake_desktop.add(11))
    tracker.sample()

    edit_window = fake_desktop.add(42, cls="Qt5152QWindowIcon")
    fake_desktop.focus(edit_window)

    assert tracker.sample() is True
    assert tracker.target_hwnd == edit_window


@pytest.mark.parametrize("shell_class", ["Shell_TrayWnd", "NotifyIconOverflowWindow", "Progman"])
def test_shell_windows_do_not_overwrite_target(fake_desktop, tracker, shell_class):
    fake_desktop.focus(fake_desktop.add(11))
    tracker.sample()

    fake_desktop.focus(fake_desktop.add(77, cls=shell_class))

    assert tracker.sample() is False
    assert tracker.target_hwnd == 11


def test_non_focusable_window_does_not_overwrite_target(fake_desktop, tracker):
    """WS_EX_NOACTIVATE 的浮窗根本收不到模拟按键，记成目标等于把粘贴丢掉。"""
    fake_desktop.focus(fake_desktop.add(11))
    tracker.sample()

    fake_desktop.focus(fake_desktop.add(66, focusable=False))

    assert tracker.sample() is False
    assert tracker.target_hwnd == 11


def test_dead_foreground_does_not_overwrite_target(fake_desktop, tracker):
    fake_desktop.focus(fake_desktop.add(11))
    tracker.sample()

    fake_desktop.focus(4242)  # 没登记过，等同于句柄已失效

    assert tracker.sample() is False
    assert tracker.target_hwnd == 11


def test_target_is_dropped_once_its_window_is_gone(fake_desktop, tracker):
    """句柄会被系统回收，指向已关闭的窗口比没有目标更危险。"""
    fake_desktop.focus(fake_desktop.add(11))
    tracker.sample()

    fake_desktop.destroy(11)

    assert tracker.target_hwnd is None


def test_switching_between_external_windows_follows_focus(fake_desktop, tracker):
    fake_desktop.focus(fake_desktop.add(11))
    tracker.sample()

    fake_desktop.focus(fake_desktop.add(22, cls="Chrome_WidgetWin_1"))

    assert tracker.sample() is True
    assert tracker.target_hwnd == 22


def test_nothing_is_excluded_before_the_window_registers_itself(fake_desktop):
    """拾取窗口构造完才注册判断，在那之前不能误把任何窗口挡掉。"""
    instance = ForegroundWindowTracker()
    fake_desktop.focus(fake_desktop.add(11))

    assert instance.sample() is True
    assert instance.target_hwnd == 11


def test_set_interval_before_start_configures_the_timer(fake_desktop, qapp):
    """设置页保存的频率要在窗口下次显示、定时器创建时就生效。"""
    fake_desktop.focus(fake_desktop.add(11))
    instance = ForegroundWindowTracker()
    instance.set_interval(1500)

    instance.start()
    try:
        assert instance._timer.interval() == 1500
    finally:
        instance.stop()


def test_set_interval_updates_a_running_timer_live(fake_desktop, qapp):
    """面板开着的时候改设置也要立即生效，不用等下次打开面板。"""
    fake_desktop.focus(fake_desktop.add(11))
    instance = ForegroundWindowTracker()
    instance.start()
    try:
        instance.set_interval(800)
        assert instance._timer.interval() == 800
    finally:
        instance.stop()


def test_sample_never_raises_into_the_timer(fake_desktop):
    """取样由定时器驱动，异常逃出去会终止进程。

    排除判断由窗口侧注入，窗口销毁后 Qt 包装对象会抛 RuntimeError。
    """
    instance = ForegroundWindowTracker()

    def _dead_window(_hwnd):
        raise RuntimeError("wrapped C/C++ object has been deleted")

    instance.set_excluded(_dead_window)
    fake_desktop.focus(fake_desktop.add(11))

    assert instance.sample() is False
    assert instance.target_hwnd is None
