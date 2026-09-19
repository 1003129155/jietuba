# -*- coding: utf-8 -*-
"""切焦点再发 Ctrl+V：按键必须等目标窗口真的到前台之后才发出。

按键事件没有收件人，早发一步就落进剪贴板窗口自己的搜索框；而一直等不到也不能
卡住不粘——重试有上限，到点按当前焦点发出去。
"""

import pytest

from clipboard.controllers import paste_keystroke as paste_module


@pytest.fixture
def stub_win32(monkeypatch):
    """把 Win32 调用和定时器换成可观测的替身，定时重试改为立即执行。"""
    state = {"foreground": 0, "activate_calls": [], "events": []}

    monkeypatch.setattr(paste_module.QTimer, "singleShot", staticmethod(lambda _ms, fn: fn()))
    monkeypatch.setattr(paste_module, "get_foreground_hwnd", lambda: state["foreground"])
    monkeypatch.setattr(paste_module, "release_modifiers", lambda: state["events"].append("release"))
    monkeypatch.setattr(paste_module, "send_ctrl_v", lambda: state["events"].append("ctrl_v"))

    def activate(hwnd):
        state["activate_calls"].append(hwnd)
        switch_after = state.get("switch_after")
        if switch_after is not None and len(state["activate_calls"]) >= switch_after:
            state["foreground"] = hwnd
        return True

    monkeypatch.setattr(paste_module, "set_foreground_window", activate)
    return state


def test_no_activation_when_target_already_foreground(stub_win32):
    stub_win32["foreground"] = 11

    paste_module.paste_to_target(11)

    assert stub_win32["activate_calls"] == []
    assert stub_win32["events"] == ["release", "ctrl_v"]


def test_waits_until_target_reaches_foreground(stub_win32):
    """第一次激活没生效，第二次才切过去——期间不能把按键发出去。"""
    stub_win32["switch_after"] = 2

    paste_module.paste_to_target(22)

    assert stub_win32["activate_calls"] == [22, 22]
    assert stub_win32["events"] == ["release", "ctrl_v"]


def test_gives_up_after_retry_limit_but_still_pastes(stub_win32):
    """提权窗口等会拒绝被激活，等不到也得粘，否则用户按了没反应。"""
    paste_module.paste_to_target(33)

    assert len(stub_win32["activate_calls"]) == paste_module._ACTIVATE_MAX_ATTEMPTS
    assert stub_win32["events"] == ["release", "ctrl_v"]


def test_without_target_pastes_to_current_focus(stub_win32, monkeypatch):
    """目标窗口已关掉时只能发给当前前台。"""
    monkeypatch.setattr(paste_module, "_foreground_is_own_process", lambda: False)

    paste_module.paste_to_target(None)

    assert stub_win32["activate_calls"] == []
    assert stub_win32["events"] == ["release", "ctrl_v"]


def test_without_target_stays_silent_on_our_own_window(stub_win32, monkeypatch):
    """目标没了、前台又是拾取窗口，发出去只会粘进自己的搜索框。"""
    monkeypatch.setattr(paste_module, "_foreground_is_own_process", lambda: True)

    paste_module.paste_to_target(None)

    assert stub_win32["events"] == []


def test_modifiers_are_released_before_the_keystroke(stub_win32):
    """热键带 Ctrl，用户还按着时发 Ctrl+V，目标收到的是按错的组合键。"""
    stub_win32["foreground"] = 44

    paste_module.paste_to_target(44)

    assert stub_win32["events"].index("release") < stub_win32["events"].index("ctrl_v")


def test_gives_up_silently_when_the_foreground_is_our_own_window(stub_win32, monkeypatch):
    """前台还停在自己人身上时盲发按键，会被本应用的快捷键处理器接住。"""
    monkeypatch.setattr(paste_module, "_foreground_is_own_process", lambda: True)

    paste_module.paste_to_target(55)

    assert len(stub_win32["activate_calls"]) == paste_module._ACTIVATE_MAX_ATTEMPTS
    assert stub_win32["events"] == []
