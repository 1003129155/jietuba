# -*- coding: utf-8 -*-
"""「粘贴后移到最前」必须由界面自己搬行，不能靠一次全量重载顺带带出来。

后端 paste_item 改的是数据库里的 item_order，界面不会自己知道。以前这件事一直
是白拿的：粘贴必然关窗，下次打开必然重查。窗口可以常驻之后这个保证没了，再指望
剪贴板监听碰巧触发一次刷新，就会时灵时不灵。
"""

from datetime import datetime, timedelta

import pytest

import settings
from clipboard.controllers import clipboard_controller as controller_module
from clipboard.controllers.clipboard_controller import ClipboardController
from clipboard.core import ClipboardItem


def _item(item_id: int, *, pinned: bool = False) -> ClipboardItem:
    return ClipboardItem(
        id=item_id,
        content=f"内容 {item_id}",
        content_type="text",
        is_pinned=pinned,
        created_at=datetime(2026, 9, 19) + timedelta(minutes=item_id),
    )


@pytest.fixture
def controller(monkeypatch):
    monkeypatch.setattr(ClipboardController, "_load_settings", lambda self: None)
    ctrl = ClipboardController(object())
    ctrl.current_items = [_item(5), _item(4), _item(3), _item(2)]
    return ctrl


@pytest.fixture
def moved(controller):
    events = []
    controller.item_moved_to_top.connect(lambda item_id, row: events.append((item_id, row)))
    return events


def test_item_moves_to_the_front(controller, moved):
    controller.move_item_to_top(3)

    assert [i.id for i in controller.current_items] == [3, 5, 4, 2]
    assert moved == [(3, 0)]


def test_item_stops_below_the_pinned_block(controller, moved):
    controller.current_items = [_item(9, pinned=True), _item(5), _item(4), _item(3)]

    controller.move_item_to_top(3)

    assert [i.id for i in controller.current_items] == [9, 3, 5, 4]
    assert moved == [(3, 1)]


def test_item_already_in_place_is_left_alone(controller, moved):
    controller.move_item_to_top(5)

    assert [i.id for i in controller.current_items] == [5, 4, 3, 2]
    assert moved == []


def test_pinned_item_does_not_move(controller, moved):
    """置顶项排在 item_order 之前，顺序不受影响。"""
    controller.current_items = [_item(9, pinned=True), _item(8, pinned=True), _item(5)]

    controller.move_item_to_top(8)

    assert [i.id for i in controller.current_items] == [9, 8, 5]
    assert moved == []


def test_item_outside_the_loaded_pages_is_ignored(controller, moved):
    controller.move_item_to_top(999)

    assert [i.id for i in controller.current_items] == [5, 4, 3, 2]
    assert moved == []


# ── paste_item 是否真的接上了 ──

class _Manager:
    def __init__(self):
        self.calls = []

    def paste_item(self, item_id, with_html, move_to_top):
        self.calls.append(move_to_top)
        return True


@pytest.fixture
def pasting(controller, monkeypatch):
    monkeypatch.setattr(controller_module.QTimer, "singleShot", lambda _ms, fn: fn())
    monkeypatch.setattr(controller_module, "paste_to_target", lambda hwnd: None)

    class _Config:
        def get_clipboard_move_to_top_on_paste(self):
            return True

    monkeypatch.setattr(settings, "get_tool_settings_manager", lambda: _Config())
    controller.manager = _Manager()
    return controller


def test_paste_reorders_the_list_without_a_reload(pasting, moved, monkeypatch):
    reloads = []
    monkeypatch.setattr(pasting, "load_history", lambda: reloads.append(1))

    assert pasting.paste_item(3) is True

    assert [i.id for i in pasting.current_items] == [3, 5, 4, 2]
    assert moved == [(3, 0)]
    assert reloads == []


def test_paste_inside_a_group_keeps_the_order(pasting, moved):
    """分组视图里粘贴不改顺序，界面也不该搬行。"""
    pasting.current_group_id = 7

    assert pasting.paste_item(3) is True

    assert pasting.manager.calls == [False]
    assert [i.id for i in pasting.current_items] == [5, 4, 3, 2]
    assert moved == []


def test_selection_index_follows_the_moved_row():
    from clipboard.controllers.selection_manager import SelectionManager

    manager = SelectionManager.__new__(SelectionManager)

    manager._selected_index = 3
    manager.shift_selection_after_move(3, 0)      # 选中的就是被搬走那行
    assert manager._selected_index == 0

    manager._selected_index = 1
    manager.shift_selection_after_move(3, 0)      # 被搬的行原先在下方，选中项下移一格
    assert manager._selected_index == 2

    manager._selected_index = 5
    manager.shift_selection_after_move(3, 0)      # 被搬的行在选中项上方，不受影响
    assert manager._selected_index == 5
