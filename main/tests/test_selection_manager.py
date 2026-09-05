# -*- coding: utf-8 -*-
"""
剪贴板列表选择管理器测试

SelectionManager 管着剪贴板窗口里"当前选中哪一条"的全部状态：
键盘上下键导航、鼠标点击、悬停起点、边界钳制。202 条语句此前只覆盖了 15%。

这类"看似简单的索引移动"最容易出错的地方在首次按键：
窗口刚呼出、还没有任何选中时，按下键应落到第一条、按上键应落到最后一条；
如果鼠标正悬停在某条上，则应从那条开始移动。这些分支各走各的路径，
测试用真实的 QListWidget 驱动，避免把实现细节写死在替身里。
"""
import pytest
from PySide6.QtCore import Qt, QEvent
from PySide6.QtGui import QKeyEvent
from PySide6.QtWidgets import QApplication, QListWidget, QListWidgetItem


@pytest.fixture(scope="module")
def qapp():
    app = QApplication.instance()
    if app is None:
        app = QApplication([])
    yield app


@pytest.fixture(autouse=True)
def _silence_preview(monkeypatch, qapp):
    """预览弹窗需要真实屏幕定位，这里替换掉，专注测选择状态本身"""
    from clipboard.ui.widgets.preview_popup import PreviewPopup

    class _NullPopup:
        def hide_preview(self):
            pass

        def show_preview(self, *args, **kwargs):
            pass

        def schedule_preview(self, *args, **kwargs):
            pass

    monkeypatch.setattr(PreviewPopup, "instance", staticmethod(lambda: _NullPopup()))


@pytest.fixture
def widget(qapp):
    w = QListWidget()
    yield w
    w.deleteLater()


def _fill(widget, count):
    """往列表里放 count 条，item_id 为 100, 101, ..."""
    for i in range(count):
        item = QListWidgetItem(f"item-{i}")
        item.setData(Qt.ItemDataRole.UserRole, 100 + i)
        widget.addItem(item)


@pytest.fixture
def manager(widget):
    from clipboard.controllers.selection_manager import SelectionManager
    _fill(widget, 5)
    return SelectionManager(widget, get_item_data=lambda idx: None)


@pytest.fixture
def empty_manager(widget):
    from clipboard.controllers.selection_manager import SelectionManager
    return SelectionManager(widget, get_item_data=lambda idx: None)


# ============================================================================
# 初始状态
# ============================================================================

class TestInitialState:
    def test_nothing_is_selected_initially(self, manager):
        assert manager.has_selection() is False
        assert manager.get_current_item_id() is None

    def test_keyboard_navigation_has_not_started(self, manager):
        assert manager._keyboard_navigation_started is False


# ============================================================================
# 首次按方向键
# ============================================================================

class TestFirstArrowKey:
    """
    窗口刚呼出时还没有选中项，首次按键决定从哪儿开始，
    这是整个导航里分支最多、也最容易写错的一段。
    """

    def test_first_down_selects_the_first_row(self, manager):
        manager._move_selection(1)
        assert manager._selected_index == 0
        assert manager.get_current_item_id() == 100

    def test_first_up_selects_the_last_row(self, manager):
        """从底部开始向上，符合"最近的记录在最下面"的直觉"""
        manager._move_selection(-1)
        assert manager._selected_index == 4
        assert manager.get_current_item_id() == 104

    def test_hovered_row_becomes_the_starting_point(self, manager):
        """鼠标停在哪条，键盘就从哪条继续，而不是跳回列表两端"""
        manager.set_hovered_index(2)
        manager._move_selection(1)
        assert manager._selected_index == 3

    def test_hovered_row_also_works_upwards(self, manager):
        manager.set_hovered_index(2)
        manager._move_selection(-1)
        assert manager._selected_index == 1

    def test_first_key_marks_navigation_as_started(self, manager):
        manager._move_selection(1)
        assert manager._keyboard_navigation_started is True


# ============================================================================
# 持续导航
# ============================================================================

class TestSubsequentNavigation:
    def test_repeated_down_walks_the_list(self, manager):
        for expected in (0, 1, 2, 3, 4):
            manager._move_selection(1)
            assert manager._selected_index == expected

    def test_repeated_up_walks_back(self, manager):
        manager._move_selection(-1)          # 落到最后一条
        for expected in (3, 2, 1, 0):
            manager._move_selection(-1)
            assert manager._selected_index == expected

    def test_selection_stops_at_the_bottom_without_wrapping(self, manager):
        """走到底再按下键应停住，不能绕回第一条"""
        for _ in range(10):
            manager._move_selection(1)
        assert manager._selected_index == 4

    def test_selection_stops_at_the_top_without_wrapping(self, manager):
        manager._move_selection(1)
        for _ in range(10):
            manager._move_selection(-1)
        assert manager._selected_index == 0

    def test_direction_can_be_reversed_midway(self, manager):
        manager._move_selection(1)
        manager._move_selection(1)
        manager._move_selection(-1)
        assert manager._selected_index == 0


class TestEmptyList:
    def test_navigation_on_an_empty_list_is_a_no_op(self, empty_manager):
        empty_manager._move_selection(1)
        empty_manager._move_selection(-1)
        assert empty_manager._selected_index == -1
        assert empty_manager.has_selection() is False


# ============================================================================
# 直接选中
# ============================================================================

class TestSelectByIndexAndId:
    def test_select_first_and_last(self, manager):
        manager.select_first()
        assert manager.get_current_item_id() == 100

        manager.select_last()
        assert manager.get_current_item_id() == 104

    def test_select_by_item_id(self, manager):
        assert manager.select_item_id(102) is True
        assert manager._selected_index == 2

    def test_selecting_an_unknown_id_fails_without_changing_state(self, manager):
        manager.select_item_id(102)
        assert manager.select_item_id(999) is False
        assert manager._selected_index == 2      # 保持原选中不变

    def test_selecting_none_is_rejected(self, manager):
        assert manager.select_item_id(None) is False

    def test_out_of_range_index_is_ignored(self, manager):
        manager._select_index(0)
        manager._select_index(99)
        manager._select_index(-5)
        assert manager._selected_index == 0

    def test_selection_change_emits_the_item_id(self, manager):
        received = []
        manager.selection_changed.connect(received.append)

        manager.select_item_id(103)

        assert received == [103]


# ============================================================================
# 清除与重置
# ============================================================================

class TestClearAndReset:
    def test_clear_selection_drops_the_current_row(self, manager):
        manager._move_selection(1)
        manager.clear_selection()

        assert manager.has_selection() is False
        assert manager.get_current_item_id() is None

    def test_clear_selection_keeps_navigation_started_by_default(self, manager):
        """
        默认不重置键盘状态：刷新列表后继续按方向键应接着走，
        而不是又回到"首次按键"的行为。
        """
        manager._move_selection(1)
        manager.clear_selection()
        assert manager._keyboard_navigation_started is True

    def test_clear_selection_can_also_reset_navigation(self, manager):
        manager._move_selection(1)
        manager.clear_selection(reset_keyboard_state=True)
        assert manager._keyboard_navigation_started is False

    def test_reset_restores_the_initial_state(self, manager):
        manager._move_selection(1)
        manager.set_hovered_index(3)

        manager.reset()

        assert manager._selected_index == -1
        assert manager._hovered_index == -1
        assert manager._keyboard_navigation_started is False
        assert manager.get_current_item_id() is None

    def test_after_reset_the_first_key_behaves_like_a_fresh_window(self, manager):
        manager._move_selection(1)
        manager._move_selection(1)
        manager.reset()

        manager._move_selection(-1)
        assert manager._selected_index == 4      # 又回到"上键选最后一条"


# ============================================================================
# 悬停索引
# ============================================================================

class TestHoveredIndex:
    def test_hover_can_be_set_and_cleared(self, manager):
        manager.set_hovered_index(2)
        assert manager._hovered_index == 2

        manager.clear_hovered_index()
        assert manager._hovered_index == -1

    def test_hover_is_only_used_before_navigation_starts(self, manager):
        """已经在用键盘导航时，鼠标划过不应把选中位置拽走"""
        manager._move_selection(1)          # 选中第 0 条，导航已开始
        manager.set_hovered_index(4)
        manager._move_selection(1)
        assert manager._selected_index == 1


# ============================================================================
# 键盘事件
# ============================================================================

class TestKeyEventHandling:
    @staticmethod
    def _press(manager, widget, key):
        event = QKeyEvent(QEvent.Type.KeyPress, key, Qt.KeyboardModifier.NoModifier)
        return manager.eventFilter(widget, event)

    def test_down_arrow_moves_the_selection(self, manager, widget):
        assert self._press(manager, widget, Qt.Key.Key_Down) is True
        assert manager._selected_index == 0

    def test_up_arrow_moves_the_selection(self, manager, widget):
        assert self._press(manager, widget, Qt.Key.Key_Up) is True
        assert manager._selected_index == 4

    def test_enter_activates_the_current_item(self, manager, widget):
        activated = []
        manager.item_activated.connect(activated.append)

        self._press(manager, widget, Qt.Key.Key_Down)
        self._press(manager, widget, Qt.Key.Key_Return)

        assert activated == [100]

    def test_enter_without_a_selection_activates_nothing(self, manager, widget):
        activated = []
        manager.item_activated.connect(activated.append)

        self._press(manager, widget, Qt.Key.Key_Return)

        assert activated == []

    def test_unrelated_keys_are_passed_through(self, manager, widget):
        """普通字符要放行，否则在列表上没法输入搜索词"""
        assert self._press(manager, widget, Qt.Key.Key_A) is False
