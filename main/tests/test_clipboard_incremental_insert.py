# -*- coding: utf-8 -*-
"""新内容到达时优先单条插入，不要整表重建。

常驻窗口下全量重载会把用户正在做的事全部打断：滚动位置回顶、选中丢失、预览被关。
但插入只在"位置算得准"的前提下才安全——视图一旦带上分组、搜索或时间筛选，
新条目属不属于这张列表、排在第几行都得由查询决定，那就必须退回全量重载。
"""

from datetime import datetime, timedelta

import pytest

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
    ctrl.current_items = [_item(3), _item(2), _item(1)]
    ctrl._current_offset = 3
    return ctrl


@pytest.fixture
def inserted(controller):
    events = []
    controller.item_inserted.connect(lambda item, row: events.append((item.id, row)))
    return events


def test_new_item_goes_to_the_top_of_the_history_view(controller, inserted):
    assert controller.insert_item(_item(4)) is True

    assert [i.id for i in controller.current_items] == [4, 3, 2, 1]
    assert inserted == [(4, 0)]


def test_new_item_lands_below_the_pinned_block(controller, inserted):
    """排序是 is_pinned DESC，插到第 0 行会跑到置顶项前面去。"""
    controller.current_items = [_item(9, pinned=True), _item(8, pinned=True), _item(3), _item(2)]

    assert controller.insert_item(_item(10)) is True

    assert [i.id for i in controller.current_items] == [9, 8, 10, 3, 2]
    assert inserted == [(10, 2)]


def test_offset_follows_the_inserted_row(controller):
    """分页按偏移量取数，头部多一行不跟着后移，下一页会重复取到交界那条。"""
    controller.insert_item(_item(4))

    assert controller._current_offset == 4


def test_duplicate_content_falls_back_to_reload(controller, inserted):
    """重复内容会被后端移到最前而不是新增，旧行还在列表里。"""
    assert controller.insert_item(_item(2)) is False
    assert inserted == []


@pytest.mark.parametrize(
    "attr, value",
    [
        ("current_group_id", 7),
        ("_search_text", "关键词"),
        ("_content_type", "image"),
        ("_time_range", (datetime(2026, 9, 1), datetime(2026, 9, 19))),
    ],
)
def test_filtered_views_fall_back_to_reload(controller, inserted, attr, value):
    setattr(controller, attr, value)

    assert controller.insert_item(_item(4)) is False
    assert inserted == []


def test_insert_is_skipped_while_a_page_is_loading(controller, inserted):
    controller._is_loading = True

    assert controller.insert_item(_item(4)) is False
    assert inserted == []


def test_hidden_window_is_left_untouched(controller, monkeypatch):
    reloads = []
    monkeypatch.setattr(controller, "load_history", lambda: reloads.append(1))

    controller.on_new_content(False, _item(4))

    assert reloads == []
    assert [i.id for i in controller.current_items] == [3, 2, 1]


def test_successful_insert_skips_the_full_reload(controller, monkeypatch):
    reloads = []
    monkeypatch.setattr(controller, "load_history", lambda: reloads.append(1))

    controller.on_new_content(True, _item(4))

    assert reloads == []


def test_reload_still_runs_when_the_item_cannot_be_placed(controller, monkeypatch):
    reloads = []
    monkeypatch.setattr(controller, "load_history", lambda: reloads.append(1))
    controller.current_group_id = 7

    controller.on_new_content(True, _item(4))

    assert reloads == [1]


def test_reload_still_runs_without_an_item(controller, monkeypatch):
    """监听回调之外的刷新入口拿不到条目，只能整表重查。"""
    reloads = []
    monkeypatch.setattr(controller, "load_history", lambda: reloads.append(1))

    controller.on_new_content(True, None)

    assert reloads == [1]


def test_selection_index_follows_the_inserted_row():
    """currentItem 不变时 Qt 不发 currentItemChanged，行号却已经变了。"""
    from clipboard.controllers.selection_manager import SelectionManager

    manager = SelectionManager.__new__(SelectionManager)
    manager._selected_index = 2

    manager.shift_selection_after_insert(0)
    assert manager._selected_index == 3

    manager.shift_selection_after_insert(5)  # 插在选中项下方，下标不受影响
    assert manager._selected_index == 3
