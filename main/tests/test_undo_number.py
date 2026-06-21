# -*- coding: utf-8 -*-
"""
序号撤销/重做命令单元测试

测试 77eee29..HEAD 范围内新增的撤销命令：
- AddNumberCommand
- RemoveNumberCommand
- RemoveNumberAndRenumberCommand
- NumberEditCommand
- BatchRemoveCommand (序号支持)
"""
import pytest
from PySide6.QtWidgets import QApplication, QGraphicsScene
from PySide6.QtCore import QPointF, QRectF
from PySide6.QtGui import QColor


@pytest.fixture(scope="module")
def qapp():
    app = QApplication.instance()
    if app is None:
        app = QApplication([])
    yield app


@pytest.fixture
def scene(qapp):
    s = QGraphicsScene()
    yield s
    s.clear()
    s.deleteLater()


@pytest.fixture
def undo_stack(qapp):
    from canvas.undo import CommandUndoStack
    return CommandUndoStack()


def _make_number_item(number: int, x: float = 0, y: float = 0):
    from canvas.items import NumberItem
    return NumberItem(number, QPointF(x, y), 20, QColor(255, 0, 0))


def _count_number_items(scene):
    from canvas.items import NumberItem
    return sum(1 for item in scene.items() if isinstance(item, NumberItem))


# ============================================================================
# AddNumberCommand 测试
# ============================================================================

class TestAddNumberCommand:
    """AddNumberCommand 测试"""

    def test_redo_adds_item_and_updates_counter(self, scene, undo_stack):
        from canvas.undo import AddNumberCommand
        from tools.number import NumberTool

        item = _make_number_item(1)
        next_before = NumberTool.get_next_number(scene)
        cmd = AddNumberCommand(scene, item, next_before=next_before)
        undo_stack.push_command(cmd)

        assert _count_number_items(scene) == 1
        assert item.scene() == scene
        # 添加后 next 应为 item.number + 1
        assert NumberTool.get_next_number(scene) == 2

    def test_undo_removes_item_and_restores_counter(self, scene, undo_stack):
        from canvas.undo import AddNumberCommand
        from tools.number import NumberTool

        next_before = NumberTool.get_next_number(scene)
        item = _make_number_item(5)
        cmd = AddNumberCommand(scene, item, next_before=next_before)
        undo_stack.push_command(cmd)

        undo_stack.undo()

        assert _count_number_items(scene) == 0
        assert NumberTool.get_next_number(scene) == next_before

    def test_redo_after_undo(self, scene, undo_stack):
        from canvas.undo import AddNumberCommand
        from tools.number import NumberTool

        item = _make_number_item(3)
        cmd = AddNumberCommand(scene, item, next_before=1)
        undo_stack.push_command(cmd)
        undo_stack.undo()
        undo_stack.redo()

        assert _count_number_items(scene) == 1
        assert NumberTool.get_next_number(scene) == 4

    def test_auto_calculates_next_when_none(self, scene, undo_stack):
        from canvas.undo import AddNumberCommand
        from tools.number import NumberTool

        item = _make_number_item(1)
        cmd = AddNumberCommand(scene, item)  # next_before/next_after = None
        undo_stack.push_command(cmd)

        assert _count_number_items(scene) == 1
        assert NumberTool.get_next_number(scene) == 2


# ============================================================================
# RemoveNumberCommand 测试
# ============================================================================

class TestRemoveNumberCommand:
    """RemoveNumberCommand 测试"""

    def test_redo_removes_item_but_preserves_counter(self, scene, undo_stack):
        from canvas.undo import AddNumberCommand, RemoveNumberCommand
        from tools.number import NumberTool

        # 先添加两个序号
        item1 = _make_number_item(1)
        item2 = _make_number_item(2, x=30)
        undo_stack.push_command(AddNumberCommand(scene, item1, next_before=1))
        undo_stack.push_command(AddNumberCommand(scene, item2, next_before=2))

        next_before_remove = NumberTool.get_next_number(scene)
        cmd = RemoveNumberCommand(scene, item2)
        undo_stack.push_command(cmd)

        assert _count_number_items(scene) == 1
        # 删除后计数器应保持（不重置）
        assert NumberTool.get_next_number(scene) == next_before_remove

    def test_undo_restores_item_and_preserves_counter(self, scene, undo_stack):
        from canvas.undo import AddNumberCommand, RemoveNumberCommand
        from tools.number import NumberTool

        item = _make_number_item(1)
        undo_stack.push_command(AddNumberCommand(scene, item, next_before=1))

        next_before_remove = NumberTool.get_next_number(scene)
        cmd = RemoveNumberCommand(scene, item)
        undo_stack.push_command(cmd)
        undo_stack.undo()

        assert _count_number_items(scene) == 1
        assert item.scene() == scene
        # 撤销后计数恢复
        assert NumberTool.get_next_number(scene) == next_before_remove


# ============================================================================
# RemoveNumberAndRenumberCommand 测试
# ============================================================================

class TestRemoveNumberAndRenumberCommand:
    """RemoveNumberAndRenumberCommand 测试"""

    def test_removes_and_renumbers_remaining(self, scene, undo_stack):
        from canvas.undo import AddNumberCommand, RemoveNumberAndRenumberCommand
        from tools.number import NumberTool

        # 创建 1, 2, 3 三个序号
        items = []
        for i, n in enumerate((1, 2, 3)):
            item = _make_number_item(n, x=i * 30)
            scene.addItem(item)
            # 分配 order
            NumberTool.assign_number_order(scene, item)
            items.append(item)

        # 重排删除 item[1]（值为 2）
        cmd = RemoveNumberAndRenumberCommand(scene, items[1])
        undo_stack.push_command(cmd)

        # 剩余 item 应被重排为 1, 2
        remaining = [it for it in scene.items()
                     if hasattr(it, "number") and it.scene() == scene]
        numbers = sorted(it.number for it in remaining)
        assert numbers == [1, 2]
        assert NumberTool.get_next_number(scene) == 3

    def test_undo_restores_original_numbers(self, scene, undo_stack):
        from canvas.undo import AddNumberCommand, RemoveNumberAndRenumberCommand
        from tools.number import NumberTool

        items = []
        for i, n in enumerate((1, 2, 3)):
            item = _make_number_item(n, x=i * 30)
            scene.addItem(item)
            NumberTool.assign_number_order(scene, item)
            items.append(item)

        next_before = NumberTool.get_next_number(scene)
        cmd = RemoveNumberAndRenumberCommand(scene, items[1])
        undo_stack.push_command(cmd)
        undo_stack.undo()

        # 恢复后应有 3 个 item，数字为原始值
        remaining = [it for it in scene.items()
                     if hasattr(it, "number") and it.scene() == scene]
        numbers = sorted(it.number for it in remaining)
        assert numbers == [1, 2, 3]
        assert NumberTool.get_next_number(scene) == next_before

    def test_remove_last_renumbers_correctly(self, scene, undo_stack):
        from canvas.undo import RemoveNumberAndRenumberCommand
        from tools.number import NumberTool

        items = []
        for i, n in enumerate((5, 10, 15)):
            item = _make_number_item(n, x=i * 30)
            scene.addItem(item)
            NumberTool.assign_number_order(scene, item)
            items.append(item)

        # 删除值为 15 的（最后一个），剩余 5,10，min=5 → 5,6
        cmd = RemoveNumberAndRenumberCommand(scene, items[2])
        undo_stack.push_command(cmd)

        numbers = sorted(
            it.number for it in scene.items()
            if hasattr(it, "number") and it.scene() == scene
        )
        assert numbers == [5, 6]
        assert NumberTool.get_next_number(scene) == 7

    def test_remove_middle_preserves_start_value(self, scene, undo_stack):
        """X 掉中间序号后从剩余最小值开始排，不是从 1。"""
        from canvas.undo import RemoveNumberAndRenumberCommand
        from tools.number import NumberTool

        items = []
        for i, n in enumerate((7, 8, 9)):
            item = _make_number_item(n, x=i * 30)
            scene.addItem(item)
            NumberTool.assign_number_order(scene, item)
            items.append(item)

        # X 8 → 剩余 7,9，min=7 → 7,8（不是 1,2）
        cmd = RemoveNumberAndRenumberCommand(scene, items[1])
        undo_stack.push_command(cmd)

        numbers = sorted(
            it.number for it in scene.items()
            if hasattr(it, "number") and it.scene() == scene
        )
        assert numbers == [7, 8]
        assert NumberTool.get_next_number(scene) == 9

    def test_remove_first_preserves_start_value(self, scene, undo_stack):
        """X 掉第一个序号后从剩余最小值开始。"""
        from canvas.undo import RemoveNumberAndRenumberCommand
        from tools.number import NumberTool

        items = []
        for i, n in enumerate((10, 20, 30)):
            item = _make_number_item(n, x=i * 30)
            scene.addItem(item)
            NumberTool.assign_number_order(scene, item)
            items.append(item)

        # X 10 → 剩余 20,30，min=20 → 20,21
        cmd = RemoveNumberAndRenumberCommand(scene, items[0])
        undo_stack.push_command(cmd)

        numbers = sorted(
            it.number for it in scene.items()
            if hasattr(it, "number") and it.scene() == scene
        )
        assert numbers == [20, 21]


# ============================================================================
# NumberEditCommand 测试
# ============================================================================

class TestNumberEditCommand:
    """NumberEditCommand 测试"""

    def test_increment_changes_number(self, scene, undo_stack):
        from canvas.undo import NumberEditCommand
        from tools.number import NumberTool

        item = _make_number_item(3)
        scene.addItem(item)

        old_state = {"number": 3}
        new_state = {"number": 4}
        next_before = NumberTool.get_next_number(scene)
        next_after = next_before  # 未超过当前 next，保持不变

        cmd = NumberEditCommand(item, old_state, new_state,
                                next_before=next_before, next_after=next_after)
        undo_stack.push_command(cmd)

        assert item.number == 4

    def test_undo_restores_old_number(self, scene, undo_stack):
        from canvas.undo import NumberEditCommand
        from tools.number import NumberTool

        item = _make_number_item(3)
        scene.addItem(item)

        next_before = NumberTool.get_next_number(scene)
        cmd = NumberEditCommand(item, {"number": 3}, {"number": 10},
                                next_before=next_before, next_after=11)
        undo_stack.push_command(cmd)
        undo_stack.undo()

        assert item.number == 3
        assert NumberTool.get_next_number(scene) == next_before

    def test_merge_within_window(self, scene, undo_stack):
        """0.7 秒内连续编辑同一 item 应合并"""
        from canvas.undo import NumberEditCommand
        from tools.number import NumberTool
        import time

        item = _make_number_item(3)
        scene.addItem(item)

        next_before = NumberTool.get_next_number(scene)
        cmd1 = NumberEditCommand(item, {"number": 3}, {"number": 4},
                                 next_before=next_before, next_after=next_before)
        undo_stack.push_command(cmd1)

        # 立即再编辑
        cmd2 = NumberEditCommand(item, {"number": 4}, {"number": 5},
                                 next_before=next_before, next_after=next_before)
        # 模拟 mergeWith 被 QUndoStack 调用
        merged = cmd1.mergeWith(cmd2)
        assert merged is True
        assert cmd1.new_state["number"] == 5

    def test_merge_different_items_returns_false(self, scene, undo_stack):
        from canvas.undo import NumberEditCommand
        from tools.number import NumberTool

        item1 = _make_number_item(3, x=0)
        item2 = _make_number_item(5, x=30)
        scene.addItem(item1)
        scene.addItem(item2)

        cmd1 = NumberEditCommand(item1, {"number": 3}, {"number": 4},
                                 next_before=1, next_after=1)
        cmd2 = NumberEditCommand(item2, {"number": 5}, {"number": 6},
                                 next_before=1, next_after=1)
        assert cmd1.mergeWith(cmd2) is False

    def test_id_returns_command_id(self, scene):
        from canvas.undo import NumberEditCommand
        item = _make_number_item(1)
        scene.addItem(item)
        cmd = NumberEditCommand(item, {"number": 1}, {"number": 2},
                                next_before=1, next_after=1)
        assert cmd.id() == 0x4E554D


# ============================================================================
# BatchRemoveCommand 序号支持测试
# ============================================================================

class TestBatchRemoveWithNumbers:
    """BatchRemoveCommand 序号支持测试"""

    def test_removes_mixed_items_preserves_counter(self, scene, undo_stack):
        from canvas.undo import BatchRemoveCommand
        from canvas.items import RectItem
        from PySide6.QtCore import QRectF
        from PySide6.QtGui import QPen, QColor as Qc
        from tools.number import NumberTool

        num_item = _make_number_item(1)
        rect = RectItem(QRectF(0, 0, 20, 20), QPen(Qc(255, 0, 0)))
        scene.addItem(num_item)
        scene.addItem(rect)

        next_before = NumberTool.get_next_number(scene)
        cmd = BatchRemoveCommand(scene, [num_item, rect],
                                 number_next_before=next_before)
        undo_stack.push_command(cmd)

        assert _count_number_items(scene) == 0
        # 计数器保持 next_before
        assert NumberTool.get_next_number(scene) == next_before

    def test_undo_restores_items_and_counter(self, scene, undo_stack):
        from canvas.undo import BatchRemoveCommand
        from tools.number import NumberTool

        num_item = _make_number_item(1)
        scene.addItem(num_item)

        next_before = NumberTool.get_next_number(scene)
        cmd = BatchRemoveCommand(scene, [num_item],
                                 number_next_before=next_before)
        undo_stack.push_command(cmd)
        undo_stack.undo()

        assert _count_number_items(scene) == 1
        assert NumberTool.get_next_number(scene) == next_before


# ============================================================================
# RemoveNumberItemCommand 别名测试
# ============================================================================

class TestAlias:
    """别名兼容性测试"""

    def test_remove_number_item_command_is_remove_number_command(self):
        from canvas.undo import RemoveNumberItemCommand, RemoveNumberCommand
        assert RemoveNumberItemCommand is RemoveNumberCommand
