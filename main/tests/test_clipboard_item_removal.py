# -*- coding: utf-8 -*-
"""删除、移动到分组、分组内上下移都只动那一行，不整表重载。

整表重载会把分页清零、重建列表并滚回顶部：在列表下方右键操作一条内容，
视图就跳回最上面，还得重新往下翻。
"""

from datetime import datetime, timedelta
from types import SimpleNamespace

import pytest
from PySide6.QtWidgets import QListWidget, QListWidgetItem

from clipboard.controllers.clipboard_controller import ClipboardController
from clipboard.controllers.selection_manager import SelectionManager
from clipboard.core import ClipboardItem
from clipboard.ui.widgets.item_delegate import ROLE_ITEM_ID
from clipboard.ui.windows.clipboard_window import ClipboardWindow


def _item(item_id: int, *, pinned: bool = False) -> ClipboardItem:
    return ClipboardItem(
        id=item_id,
        content=f"内容 {item_id}",
        content_type="text",
        is_pinned=pinned,
        created_at=datetime(2026, 9, 19) + timedelta(minutes=item_id),
    )


class _Manager:
    def __init__(self):
        self.group_items = []
        self.deleted = []
        self.moved = []
        self.between = []

    def delete_item(self, item_id):
        self.deleted.append(item_id)
        return True

    def move_to_group(self, item_id, group_id):
        self.moved.append((item_id, group_id))
        return True

    def get_by_group(self, group_id, offset=0, limit=1000):
        return list(self.group_items)

    def move_item_between(self, item_id, before_id=None, after_id=None):
        self.between.append((item_id, before_id, after_id))
        return True


@pytest.fixture
def controller(monkeypatch):
    monkeypatch.setattr(ClipboardController, "_load_settings", lambda self: None)
    ctrl = ClipboardController(_Manager())
    ctrl.current_items = [_item(5), _item(4), _item(3), _item(2)]
    ctrl._current_offset = 4
    ctrl.reloads = []
    monkeypatch.setattr(ctrl, "load_history", lambda: ctrl.reloads.append(1))
    ctrl.removed = []
    ctrl.item_removed.connect(ctrl.removed.append)
    ctrl.row_moves = []
    ctrl.item_row_moved.connect(lambda src, dst: ctrl.row_moves.append((src, dst)))
    return ctrl


def _ids(controller):
    return [item.id for item in controller.current_items]


class TestRemoval:
    def test_deleting_removes_only_that_row(self, controller):
        assert controller.delete_item(3) is True

        assert _ids(controller) == [5, 4, 2]
        assert controller.removed == [2]
        assert controller.reloads == []
        # 下一页从少了一行的位置接着取
        assert controller._current_offset == 3

    def test_moving_to_a_group_keeps_the_row_in_history(self, controller):
        """历史视图不按分组过滤，移动后这条仍在原位。"""
        controller.move_to_group(3, 7)

        assert _ids(controller) == [5, 4, 3, 2]
        assert controller.removed == [] and controller.reloads == []
        assert controller.manager.moved == [(3, 7)]

    @pytest.mark.parametrize("target", [2, None])
    def test_moving_out_of_the_current_group_removes_the_row(self, controller, target):
        controller.current_group_id = 1

        controller.move_to_group(3, target)

        assert _ids(controller) == [5, 4, 2]
        assert controller.removed == [2]
        assert controller.reloads == []

    def test_moving_into_the_group_it_is_already_in_changes_nothing(self, controller):
        controller.current_group_id = 1

        controller.move_to_group(3, 1)

        assert _ids(controller) == [5, 4, 3, 2]
        assert controller.removed == []

    def test_item_outside_the_loaded_pages_is_ignored(self, controller):
        controller.delete_item(999)

        assert controller.removed == []
        assert controller._current_offset == 4


class TestMoveWithinGroup:
    @pytest.fixture
    def group(self, controller):
        controller.current_group_id = 1
        controller.current_items = [_item(1), _item(2), _item(3)]
        controller.manager.group_items = list(controller.current_items)
        return controller

    def test_moving_down_swaps_with_the_next_row(self, group):
        assert group.move_item_order(2, 1, 1) is True

        assert _ids(group) == [1, 3, 2]
        assert group.row_moves == [(1, 2)]
        assert group.reloads == []

    def test_moving_up_swaps_with_the_previous_row(self, group):
        group.move_item_order(3, 1, -1)

        assert _ids(group) == [1, 3, 2]
        assert group.row_moves == [(2, 1)]

    def test_crossing_the_pinned_block_does_not_move_the_row(self, group):
        """排序先按是否置顶，只改 item_order 时显示顺序不变。"""
        group.current_items = [_item(1, pinned=True), _item(2), _item(3)]
        group.manager.group_items = list(group.current_items)

        group.move_item_order(2, 1, -1)

        assert group.manager.between == [(2, None, 1)]
        assert _ids(group) == [1, 2, 3]
        assert group.row_moves == [] and group.reloads == []

    def test_search_results_fall_back_to_a_reload(self, group):
        group._search_text = "内容"

        group.move_item_order(2, 1, 1)

        assert group.reloads == [1] and group.row_moves == []

    def test_neighbour_not_loaded_yet_falls_back_to_a_reload(self, group):
        group.current_items = [_item(1), _item(2)]

        group.move_item_order(2, 1, 1)

        assert group.reloads == [1] and group.row_moves == []


def test_selection_index_follows_a_row_moved_down():
    manager = SelectionManager.__new__(SelectionManager)

    manager._selected_index = 1
    manager.shift_selection_after_move(1, 3)
    assert manager._selected_index == 3

    manager._selected_index = 2
    manager.shift_selection_after_move(1, 3)      # 被搬的行越过了选中项，选中项上移一格
    assert manager._selected_index == 1

    manager._selected_index = 4
    manager.shift_selection_after_move(1, 3)
    assert manager._selected_index == 4


def test_selection_and_hover_follow_a_removed_row():
    manager = SelectionManager.__new__(SelectionManager)
    cleared = []
    manager.clear_selection = lambda: cleared.append(1)

    manager._selected_index, manager._hovered_index = 5, 2
    manager.shift_selection_after_remove(2)
    assert (manager._selected_index, manager._hovered_index) == (4, -1)

    manager.shift_selection_after_remove(0)
    assert (manager._selected_index, manager._hovered_index) == (3, -1)
    manager._hovered_index = 6
    manager.shift_selection_after_remove(7)
    assert (manager._selected_index, manager._hovered_index) == (3, 6)
    assert cleared == []

    manager.shift_selection_after_remove(3)
    assert cleared == [1]


class TestWindowRows:
    @pytest.fixture
    def window(self, qapp):
        widget = QListWidget()
        for item_id in (10, 11, 12, 13):
            row = QListWidgetItem(str(item_id))
            row.setData(ROLE_ITEM_ID, item_id)
            widget.addItem(row)
        selection = SelectionManager(widget, lambda _item_id: None)
        yield SimpleNamespace(
            list_widget=widget,
            selection_manager=selection,
            _check_and_load_more_if_needed=lambda: None,
        )
        widget.deleteLater()

    @staticmethod
    def _row_ids(window):
        widget = window.list_widget
        return [widget.item(row).data(ROLE_ITEM_ID) for row in range(widget.count())]

    def test_removing_a_row_above_keeps_the_selected_item(self, window):
        window.selection_manager.select_item_id(12)

        ClipboardWindow._on_item_removed(window, 0)

        assert self._row_ids(window) == [11, 12, 13]
        assert window.selection_manager.get_current_item_id() == 12

    def test_removing_the_selected_row_clears_the_selection(self, window):
        window.selection_manager.select_item_id(11)

        ClipboardWindow._on_item_removed(window, 1)

        assert self._row_ids(window) == [10, 12, 13]
        assert window.selection_manager.has_selection() is False

    def test_moved_row_keeps_its_selection(self, window):
        window.selection_manager.select_item_id(11)

        ClipboardWindow._on_item_row_moved(window, 1, 2)

        assert self._row_ids(window) == [10, 12, 11, 13]
        assert window.selection_manager.get_current_item_id() == 11
