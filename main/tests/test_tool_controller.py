# -*- coding: utf-8 -*-
"""ToolController.current_tool_id —— "没有工具 = cursor" 的唯一归一化入口。

在此之前，view.py / magnifier.py / action.py / screenshot_window.py 各自手写一遍
"current_tool 为 None 时按 cursor 处理"，这个属性把它收成一处。
"""
from tools.base import ToolContext
from tools.controller import ToolController


class _FakeTool:
    def __init__(self, tool_id):
        self.id = tool_id

    def on_activate(self, ctx):
        pass

    def on_deactivate(self, ctx):
        pass


def _make_controller():
    ctx = ToolContext(
        scene=None, selection=None, undo_stack=None,
        color=None, stroke_width=1, opacity=1.0,
    )
    return ToolController(ctx)


def test_no_tool_active_reports_cursor():
    controller = _make_controller()
    assert controller.current_tool_id == "cursor"


def test_reports_the_id_of_the_active_tool():
    controller = _make_controller()
    controller.register(_FakeTool("pen"))
    controller.activate("pen")
    assert controller.current_tool_id == "pen"


def test_reverts_to_cursor_after_the_cursor_tool_is_activated():
    controller = _make_controller()
    controller.register(_FakeTool("pen"))
    controller.register(_FakeTool("cursor"))
    controller.activate("pen")
    controller.activate("cursor")
    assert controller.current_tool_id == "cursor"
