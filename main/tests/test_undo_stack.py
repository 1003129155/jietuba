# -*- coding: utf-8 -*-
"""
CommandUndoStack 撤销/重做栈单元测试

测试 push / undo / redo / 状态查询等逻辑。
"""
import pytest
from PySide6.QtWidgets import QApplication
from PySide6.QtGui import QUndoCommand


@pytest.fixture(scope="module")
def qapp():
    app = QApplication.instance()
    if app is None:
        app = QApplication([])
    yield app


class SimpleCommand(QUndoCommand):
    """用于测试的简单命令"""
    def __init__(self, log: list, text="test"):
        super().__init__(text)
        self.log = log

    def undo(self):
        self.log.append("undo")

    def redo(self):
        self.log.append("redo")


@pytest.fixture
def stack(qapp):
    from canvas.undo import CommandUndoStack
    return CommandUndoStack()


class TestCommandUndoStack:
    """撤销栈测试"""

    def test_initial_state(self, stack):
        """初始状态"""
        assert stack.count() == 0
        assert stack.index() == 0
        assert not stack.canUndo()
        assert not stack.canRedo()

    def test_push_command(self, stack):
        """推入命令"""
        log = []
        stack.push_command(SimpleCommand(log, "cmd1"))
        assert stack.count() == 1
        assert stack.index() == 1
        assert stack.canUndo()
        assert not stack.canRedo()
        # push 会自动执行 redo（首次执行）
        assert "redo" in log

    def test_undo(self, stack):
        """撤销"""
        log = []
        stack.push_command(SimpleCommand(log, "cmd1"))
        stack.undo()
        assert stack.index() == 0
        assert stack.canRedo()
        assert "undo" in log

    def test_redo(self, stack):
        """重做"""
        log = []
        stack.push_command(SimpleCommand(log, "cmd1"))
        stack.undo()
        log.clear()
        stack.redo()
        assert stack.index() == 1
        assert "redo" in log

    def test_undo_empty_stack(self, stack):
        """空栈撤销不崩溃"""
        stack.undo()  # 不应抛出异常
        assert stack.index() == 0

    def test_redo_at_top(self, stack):
        """栈顶重做不崩溃"""
        log = []
        stack.push_command(SimpleCommand(log))
        stack.redo()  # 已在栈顶
        assert stack.index() == 1

    def test_multiple_commands(self, stack):
        """多个命令的撤销重做"""
        logs = [[], [], []]
        stack.push_command(SimpleCommand(logs[0], "cmd1"))
        stack.push_command(SimpleCommand(logs[1], "cmd2"))
        stack.push_command(SimpleCommand(logs[2], "cmd3"))
        assert stack.count() == 3
        assert stack.index() == 3

        stack.undo()
        assert stack.index() == 2
        stack.undo()
        assert stack.index() == 1
        stack.redo()
        assert stack.index() == 2

    def test_push_after_undo_clears_redo(self, stack):
        """撤销后推入新命令应清除重做历史"""
        log1, log2, log3 = [], [], []
        stack.push_command(SimpleCommand(log1, "cmd1"))
        stack.push_command(SimpleCommand(log2, "cmd2"))
        stack.undo()  # 回到 cmd1
        stack.push_command(SimpleCommand(log3, "cmd3"))
        # cmd2 应该被丢弃
        assert stack.count() == 2
        assert not stack.canRedo()

    def test_print_stack_status(self, stack):
        """打印状态不崩溃"""
        stack.push_command(SimpleCommand([], "test"))
        stack.print_stack_status()  # 不应抛出异常
 