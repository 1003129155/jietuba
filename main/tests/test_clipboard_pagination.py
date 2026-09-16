# -*- coding: utf-8 -*-
"""ClipboardController 分页加载的回归测试。

分组内搜索走客户端过滤（先按分组分页取一批，再在 Python 里按关键词筛），这条路
上"是否还有下一页"必须按过滤前的条数判断，不能按过滤后的条数——否则命中关键词
的条目一旦跨页，后面的页会被误判成不存在。
"""

from clipboard.controllers.clipboard_controller import ClipboardController
from clipboard.core import ClipboardItem


class DummyClipboardManager:
    """只实现分页测试用得到的两个方法，行为对齐真实 ClipboardManager：
    按 offset/limit 返回一页原始数据，不做任何关键词过滤（过滤是控制器自己做的）。
    """

    def __init__(self, group_items=None):
        self._group_items = group_items or {}

    def get_by_group(self, group_id, offset=0, limit=50):
        items = list(self._group_items.get(group_id, []))
        return items[offset: offset + limit]


def _make_controller(monkeypatch, manager):
    monkeypatch.setattr(ClipboardController, "_load_settings", lambda self: None)
    controller = ClipboardController(manager)
    return controller


def _collect_all_pages(controller, max_pages=20):
    """模拟真实 UI 的用法：滚动到底就调一次 _load_more_items，直到 has_more_items() 为假。"""
    collected = []
    controller.data_loaded.connect(lambda new_items, is_first: collected.extend(new_items))
    for _ in range(max_pages):
        if not controller.has_more_items():
            break
        controller._load_more_items()
    return collected


def test_group_search_finds_matches_that_fall_on_a_later_page(monkeypatch):
    # 20 条，每 5 条一个命中关键词；小页强制它必须翻好几页才能收全。
    items = [
        ClipboardItem(id=i, content=f"item {i} " + ("apple" if i % 5 == 0 else "pear"),
                      content_type="text")
        for i in range(20)
    ]
    manager = DummyClipboardManager(group_items={1: items})
    controller = _make_controller(monkeypatch, manager)
    controller._page_size = 6  # 6 条/页 < 命中间隔，任何一页几乎必然被过滤掉几条
    controller.current_group_id = 1
    controller._search_text = "apple"

    collected = _collect_all_pages(controller)

    expected_ids = {item.id for item in items if "apple" in item.content}
    assert {item.id for item in collected} == expected_ids
    # 分组一共 20 条，翻页必须把它们全部走完（按 raw_count 累加），
    # 而不是在过滤后的第一页就因为「不满一页」提前停住。
    assert controller._current_offset == len(items)
    assert controller.has_more_items() is False


def test_group_search_with_no_matches_on_first_page_still_finds_later_ones(monkeypatch):
    # 极端情况：第一页整页都被过滤掉（raw_count 满页但 new_items 为空），
    # 用 len(new_items) 判断会在这里就误判成"没有更多"，直接漏掉后面全部命中。
    items = (
        [ClipboardItem(id=i, content="pear only", content_type="text") for i in range(6)]
        + [ClipboardItem(id=100 + i, content="apple hit", content_type="text") for i in range(3)]
    )
    manager = DummyClipboardManager(group_items={1: items})
    controller = _make_controller(monkeypatch, manager)
    controller._page_size = 6
    controller.current_group_id = 1
    controller._search_text = "apple"

    collected = _collect_all_pages(controller)

    assert {item.id for item in collected} == {100, 101, 102}


def test_plain_group_browsing_without_search_is_unaffected(monkeypatch):
    """不搜索时 raw_count 和 len(new_items) 本来就相等，修复前后行为一致。"""
    items = [ClipboardItem(id=i, content=f"item {i}", content_type="text") for i in range(10)]
    manager = DummyClipboardManager(group_items={1: items})
    controller = _make_controller(monkeypatch, manager)
    controller._page_size = 4
    controller.current_group_id = 1

    collected = _collect_all_pages(controller)

    assert [item.id for item in collected] == [item.id for item in items]
