"""
undo.py - QUndoStack 的矢量图形撤销/重做（命令模式）

包含：
- CommandUndoStack：带调试信息的撤销栈（push_command / undo / redo / print_stack_status）
- AddItemCommand：添加图元
- RemoveItemCommand：移除图元
- BatchRemoveCommand：批量移除图元（橡皮擦等工具）
- EditItemCommand：编辑图元（控制点拖拽/变换等），通过 old_state / new_state 回放

state 约定（EditItemCommand 支持的字段）：
- "pos": QPointF
- "transform": QTransform
- "rotation": float
- "transformOriginPoint": QPointF
- "rect": QRectF
- "start": QPointF
- "end": QPointF
"""

from __future__ import annotations

from typing import Optional, Dict, Any

from PyQt6.QtCore import QRectF, QPointF
from PyQt6.QtGui import QUndoStack, QUndoCommand, QTransform
from PyQt6.QtWidgets import QGraphicsItem, QGraphicsScene


# ============================================================================
#  Undo Stack
# ============================================================================

class CommandUndoStack(QUndoStack):
    """基于命令模式的撤销栈（带调试输出）"""

    def __init__(self, parent=None):
        super().__init__(parent)

    def push_command(self, command: QUndoCommand):
        self.push(command)

    def undo(self):
        """重写 undo：添加调试信息"""
        if self.canUndo():
            print(f"↩️ [撤销栈] 执行撤销，当前索引: {self.index()}/{self.count()}")
            print(f"    撤销命令: {self.undoText()}")
            super().undo()
            print(f"    撤销后索引: {self.index()}/{self.count()}")
        else:
            print(f"⚠️ [撤销栈] 无法撤销，栈为空或已到底部 (索引: {self.index()}/{self.count()})")

    def redo(self):
        """重写 redo：添加调试信息"""
        if self.canRedo():
            print(f"↪️ [撤销栈] 执行重做，当前索引: {self.index()}/{self.count()}")
            print(f"    重做命令: {self.redoText()}")
            super().redo()
            print(f"    重做后索引: {self.index()}/{self.count()}")
        else:
            print(f"⚠️ [撤销栈] 无法重做，已到顶部 (索引: {self.index()}/{self.count()})")

    def print_stack_status(self):
        """打印撤销栈状态"""
        print("📚 [撤销栈状态]")
        print(f"    总命令数: {self.count()}")
        print(f"    当前索引: {self.index()}")
        print(f"    可撤销: {self.canUndo()}")
        print(f"    可重做: {self.canRedo()}")
        if self.canUndo():
            print(f"    下一个撤销: {self.undoText()}")
        if self.canRedo():
            print(f"    下一个重做: {self.redoText()}")


# ============================================================================
#  Commands
# ============================================================================

class AddItemCommand(QUndoCommand):
    """添加图元命令"""

    def __init__(self, scene: QGraphicsScene, item: QGraphicsItem, text: str = "Add Item"):
        super().__init__(text)
        self.scene = scene
        self.item = item

    def undo(self):
        if self.item is not None and self.item.scene() == self.scene:
            self.scene.removeItem(self.item)

    def redo(self):
        if self.item is not None and self.item.scene() != self.scene:
            self.scene.addItem(self.item)


class RemoveItemCommand(QUndoCommand):
    """移除图元命令"""

    def __init__(self, scene: QGraphicsScene, item: QGraphicsItem, text: str = "Remove Item"):
        super().__init__(text)
        self.scene = scene
        self.item = item

    def undo(self):
        if self.item is not None and self.item.scene() != self.scene:
            self.scene.addItem(self.item)

    def redo(self):
        if self.item is not None and self.item.scene() == self.scene:
            self.scene.removeItem(self.item)


class BatchRemoveCommand(QUndoCommand):
    """批量移除图元命令（用于橡皮擦等工具）"""

    def __init__(self, scene: QGraphicsScene, items: list, text: str = "Remove Items"):
        super().__init__(text)
        self.scene = scene
        self.items = list(items)  # 复制列表避免外部修改

    def undo(self):
        """撤销 - 恢复所有被删除的图元"""
        for item in self.items:
            if item is not None and item.scene() != self.scene:
                self.scene.addItem(item)

    def redo(self):
        """重做 - 删除所有图元"""
        for item in self.items:
            if item is not None and item.scene() == self.scene:
                self.scene.removeItem(item)


class EditItemCommand(QUndoCommand):
    """
    编辑图元命令 - 用于控制点拖拽等修改操作

    参数：
    - item: QGraphicsItem
    - old_state/new_state: dict（会做一层“安全拷贝”，避免外部引用被改）
    """

    def __init__(self, item: QGraphicsItem, old_state: Dict[str, Any], new_state: Dict[str, Any], text: str = "Edit Item"):
        super().__init__(text)
        self.item = item
        self.old_state = self._clone_state(old_state or {})
        self.new_state = self._clone_state(new_state or {})

    def undo(self):
        self._apply_state(self.old_state)

    def redo(self):
        self._apply_state(self.new_state)

    # ---------------- internal ----------------

    @staticmethod
    def _clone_state(state: Dict[str, Any]) -> Dict[str, Any]:
        """拷贝 state，尽量把 Qt 值类型复制一份，避免引用复用导致撤销不稳定"""
        out: Dict[str, Any] = {}
        for k, v in state.items():
            if isinstance(v, QRectF):
                out[k] = QRectF(v)
            elif isinstance(v, QPointF):
                out[k] = QPointF(v)
            elif isinstance(v, QTransform):
                out[k] = QTransform(v)
            elif isinstance(v, (int, float, str, bool, type(None))):
                out[k] = v
            else:
                # 其他复杂对象：先原样放（如果你后续需要，也可以在这里扩展深拷贝）
                out[k] = v
        return out

    def _apply_state(self, state: Dict[str, Any]):
        """将状态应用到 item"""
        if self.item is None:
            return

        # pos
        pos = state.get("pos")
        if isinstance(pos, QPointF) and hasattr(self.item, "setPos"):
            self.item.setPos(QPointF(pos))

        # transform
        transform = state.get("transform")
        if isinstance(transform, QTransform) and hasattr(self.item, "setTransform"):
            self.item.setTransform(QTransform(transform))

        # rotation
        rotation = state.get("rotation")
        if isinstance(rotation, (int, float)) and hasattr(self.item, "setRotation"):
            self.item.setRotation(float(rotation))

        # transformOriginPoint
        origin = state.get("transformOriginPoint")
        if isinstance(origin, QPointF) and hasattr(self.item, "setTransformOriginPoint"):
            self.item.setTransformOriginPoint(QPointF(origin))

        # opacity
        opacity = state.get("opacity")
        if isinstance(opacity, (int, float)) and hasattr(self.item, "setOpacity"):
            try:
                self.item.setOpacity(float(opacity))
            except Exception:
                pass

        # rect（RectItem/EllipseItem 等）
        rect = state.get("rect")
        if isinstance(rect, QRectF):
            if hasattr(self.item, "setRect") and callable(getattr(self.item, "setRect")):
                self.item.setRect(QRectF(rect))
            elif hasattr(self.item, "rect"):
                try:
                    setattr(self.item, "rect", QRectF(rect))
                except Exception:
                    pass

        # start/end（ArrowItem / 自定义箭头）
        start = state.get("start")
        if isinstance(start, QPointF):
            # 兼容：item.start / item.start_pos
            if hasattr(self.item, "start"):
                try:
                    setattr(self.item, "start", QPointF(start))
                except Exception:
                    pass
            if hasattr(self.item, "start_pos"):
                try:
                    setattr(self.item, "start_pos", QPointF(start))
                except Exception:
                    pass

        end = state.get("end")
        if isinstance(end, QPointF):
            # 兼容：item.end / item.end_pos
            if hasattr(self.item, "end"):
                try:
                    setattr(self.item, "end", QPointF(end))
                except Exception:
                    pass
            if hasattr(self.item, "end_pos"):
                try:
                    setattr(self.item, "end_pos", QPointF(end))
                except Exception:
                    pass

        # 如果你的 item 有 update_geometry 之类的，顺便触发
        if hasattr(self.item, "update_geometry") and callable(getattr(self.item, "update_geometry")):
            try:
                self.item.update_geometry()
            except Exception:
                pass

        # 触发重绘
        if hasattr(self.item, "update"):
            self.item.update()
