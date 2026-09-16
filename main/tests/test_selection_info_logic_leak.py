# -*- coding: utf-8 -*-
"""BorderShadowLogic / RoundedCornersLogic 跨截图会话复用按钮时的事件过滤器回归测试。

截图窗口是跨会话复用的：`btn_border`/`btn_rounded` 这两个按钮本身不会随会话
销毁，只有 uninstall() 会把当次会话的 popup 处理掉。BorderShadowLogic 曾经在
uninstall() 里只 deleteLater() 了 popup，却从没把自己从按钮的事件过滤器上摘下
来——下一次会话再悬停同一个按钮，上一条会话残留的过滤器仍然会被触发，访问
已经不存在的 popup。RoundedCornersLogic 早就用 `popup.destroyed` 信号摘掉了
自己，这里补上同一套、以及一次真正验证"确实修好了"的对照：把 destroyed 信号
断开、确认用例会失败。
"""
from unittest.mock import MagicMock

from PySide6.QtCore import QEvent
from PySide6.QtWidgets import QApplication, QPushButton, QWidget

from ui.selection_info.border_shadow import BorderShadowLogic
from ui.selection_info.rounded_corners import RoundedCornersLogic
from ui.selection_info.hook_manager import HookManager


def _flush_deferred_deletes(qapp):
    """deleteLater() 安排的删除不会在普通 processEvents() 里可靠地立刻完成，
    必须专门冲一遍 DeferredDelete 事件（这个项目自己的 double_click_cases.py
    _close_test_windows 也是这么做的），否则接下来的悬停测不出任何问题。
    """
    QApplication.sendPostedEvents(None, QEvent.Type.DeferredDelete)
    qapp.processEvents()


def _hover(btn):
    """真实分发一次 Enter 再 Leave，走到 eventFilter 里会摸 popup 的那两支。"""
    QApplication.sendEvent(btn, QEvent(QEvent.Type.Enter))
    QApplication.sendEvent(btn, QEvent(QEvent.Type.Leave))


def _make_border_shadow_logic(parent, btn):
    return BorderShadowLogic(
        parent_widget=parent,
        btn_border=btn,
        selection_item=MagicMock(),
        mask_overlay=MagicMock(),
        export_service=MagicMock(),
        selection_model=MagicMock(),
        config_manager=None,
        hook_manager=HookManager(),
    )


def _make_rounded_corners_logic(parent, btn):
    return RoundedCornersLogic(
        parent_widget=parent,
        btn_rounded=btn,
        selection_item=MagicMock(),
        mask_overlay=MagicMock(),
        export_service=MagicMock(),
        selection_model=MagicMock(),
        config_manager=None,
        hook_manager=HookManager(),
    )


def test_hovering_the_reused_button_after_many_sessions_does_not_touch_a_dead_popup(qapp):
    parent = QWidget()
    btn = QPushButton(parent)  # 常驻按钮：跨"会话"复用同一个实例

    kept_alive = []  # 模拟真实场景：controller 在多次会话里持续存在，不主动丢引用
    for _ in range(5):
        logic = _make_border_shadow_logic(parent, btn)
        logic.set_enabled(True)  # _enabled=True 才会真正走到访问 popup 的分支
        kept_alive.append(logic)
        logic.uninstall()
        _flush_deferred_deletes(qapp)

    _hover(btn)  # 不该抛异常、不该被 safe_event 吞掉任何异常


def test_hovering_without_the_destroyed_signal_fix_crashes(qapp, monkeypatch):
    """对照组：把 destroyed 信号连接短路掉，确认用例真的会抓到这个问题
    （而不是因为构造方式碰巧规避了它，参见上面用例改好之前实测过的真实崩溃：
    RuntimeError: libshiboken: Internal C++ object (QTimer) already deleted）。
    """
    # PySide6 的信号 connect 是 Signal 描述符层面的方法，不便全局 monkeypatch；
    # 直接在构造后立刻断开这一条连接，效果等价于"修复没生效"。
    parent = QWidget()
    btn = QPushButton(parent)

    caught = []
    from core import crash_handler
    monkeypatch.setattr(crash_handler, "_write_crash", lambda *a, **k: caught.append(True))

    for _ in range(3):
        logic = _make_border_shadow_logic(parent, btn)
        logic.set_enabled(True)
        logic._popup.destroyed.disconnect(logic._on_popup_destroyed)
        logic.uninstall()
        _flush_deferred_deletes(qapp)

    _hover(btn)
    assert caught, "断开 destroyed 信号后，悬停应该重新触发 safe_event 捕获的崩溃"


def test_rounded_corners_logic_still_covered_by_the_same_pattern(qapp):
    """RoundedCornersLogic 是这套 destroyed-signal 修法最早的实现，顺带锁住它不回归。"""
    parent = QWidget()
    btn = QPushButton(parent)

    kept_alive = []
    for _ in range(5):
        logic = _make_rounded_corners_logic(parent, btn)
        logic.set_enabled(True)
        kept_alive.append(logic)
        logic.uninstall()
        _flush_deferred_deletes(qapp)

    _hover(btn)
