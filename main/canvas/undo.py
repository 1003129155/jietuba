"""
undo.py - QUndoStack 的矢量图形撤销/重做（命令模式）

包含：
- CommandUndoStack：带调试信息的撤销栈（push_command / undo / redo / print_stack_status）
- AddItemCommand：添加图元
- AddNumberCommand：添加序号图元并同步下一序号
- RemoveItemCommand：移除图元
- RemoveNumberCommand：移除序号图元但保持下一序号不变
- RemoveNumberAndRenumberCommand：移除序号图元并将剩余序号压缩为 1..N
- BatchRemoveCommand：批量移除图元（橡皮擦等工具）
- EditItemCommand：编辑图元（控制点拖拽/变换等），通过 old_state / new_state 回放
- NumberEditCommand：编辑序号值并同步下一序号

state 约定（EditItemCommand 支持的字段）：
- "pos": QPointF
- "transform": QTransform
- "rotation": float
- "transformOriginPoint": QPointF
- "number": int
- "rect": QRectF
- "start": QPointF
- "end": QPointF
- "smooth": bool（框选马赛克的种类：True=模糊，False=马赛克）
- "block_size" + "reduced_image": int + QImage（马赛克粒度，必须成对出现——
  block_size 变了，配套的缩小图也得跟着换成同一粒度那张）
"""

from __future__ import annotations

import time
from typing import Optional, Dict, Any

from PySide6.QtCore import QRectF, QPointF
from PySide6.QtGui import QUndoStack, QUndoCommand, QTransform
from PySide6.QtWidgets import QGraphicsItem, QGraphicsScene

from core import log_debug
from core.i18n import make_tr
from core.logger import log_exception, T

_undo_tr = make_tr("UndoCommands")

# ============================================================================
# Undo Stack
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
            log_debug(T("执行撤销，当前索引: {index}/{count}", index=self.index(), count=self.count()), "UndoStack")
            log_debug(T("撤销命令: {undo_text}", undo_text=self.undoText()), "UndoStack")
            super().undo()
            log_debug(T("撤销后索引: {index}/{count}", index=self.index(), count=self.count()), "UndoStack")
        else:
            log_debug(T("无法撤销，栈为空或已到底部 (索引: {index}/{count})", index=self.index(), count=self.count()), "UndoStack")

    def redo(self):
        """重写 redo：添加调试信息"""
        if self.canRedo():
            log_debug(T("执行重做，当前索引: {index}/{count}", index=self.index(), count=self.count()), "UndoStack")
            log_debug(T("重做命令: {redo_text}", redo_text=self.redoText()), "UndoStack")
            super().redo()
            log_debug(T("重做后索引: {index}/{count}", index=self.index(), count=self.count()), "UndoStack")
        else:
            log_debug(T("无法重做，已到顶部 (索引: {index}/{count})", index=self.index(), count=self.count()), "UndoStack")

    def print_stack_status(self):
        """打印撤销栈状态"""
        log_debug(T("撤销栈状态"), "UndoStack")
        log_debug(T("总命令数: {count}", count=self.count()), "UndoStack")
        log_debug(T("当前索引: {index}", index=self.index()), "UndoStack")
        log_debug(T("可撤销: {can_undo}", can_undo=self.canUndo()), "UndoStack")
        log_debug(T("可重做: {can_redo}", can_redo=self.canRedo()), "UndoStack")
        if self.canUndo():
            log_debug(T("下一个撤销: {undo_text}", undo_text=self.undoText()), "UndoStack")
        if self.canRedo():
            log_debug(T("下一个重做: {redo_text}", redo_text=self.redoText()), "UndoStack")


# ============================================================================
# Commands
# ============================================================================

def _is_number_item(item: QGraphicsItem) -> bool:
    try:
        from canvas.items import NumberItem
        return isinstance(item, NumberItem)
    except Exception:
        return False


def _items_contain_number(items: list) -> bool:
    return any(_is_number_item(item) for item in items)


def _get_number_next(scene: QGraphicsScene) -> Optional[int]:
    try:
        from tools.number import NumberTool
        return NumberTool.get_next_number(scene)
    except Exception as e:
        log_exception(e, T("获取序号计数器"))
        return None


def _set_number_next(scene: QGraphicsScene, next_number: Optional[int]):
    if next_number is None:
        return
    try:
        from tools.number import NumberTool
        NumberTool.set_next_number_and_refresh(scene, int(next_number))
    except Exception as e:
        log_exception(e, T("恢复序号计数器"))


def _get_number_items(scene: QGraphicsScene) -> list:
    if scene is None:
        return []
    try:
        return [item for item in scene.items() if _is_number_item(item)]
    except Exception as e:
        log_exception(e, T("获取序号图元"))
        return []


def _get_draw_order_map(scene: QGraphicsScene) -> Dict[int, int]:
    """返回接近绘制顺序的索引：越早绘制，值越小。"""
    if scene is None:
        return {}
    try:
        return {id(item): index for index, item in enumerate(reversed(scene.items()))}
    except Exception as e:
        log_exception(e, T("获取绘制顺序"))
        return {}


def _number_value(item: QGraphicsItem) -> int:
    try:
        return max(1, int(getattr(item, "number", 1)))
    except Exception:
        return 1


def _number_order_value(item: QGraphicsItem, fallback: int) -> int:
    order = getattr(item, "number_order", None)
    if isinstance(order, int) and order >= 0:
        return order
    return fallback


def _apply_number_values(values: Dict[QGraphicsItem, int]):
    for item, number in values.items():
        if item is None:
            continue
        try:
            item.number = max(1, int(number))
            if hasattr(item, "update"):
                item.update()
        except Exception as e:
            log_exception(e, T("应用序号重排"))


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


class AddNumberCommand(AddItemCommand):
    """添加序号图元，同时记录创建前后的下一序号。"""

    def __init__(
        self,
        scene: QGraphicsScene,
        item: QGraphicsItem,
        next_before: Optional[int] = None,
        next_after: Optional[int] = None,
        text: str = "Add Number",
    ):
        super().__init__(scene, item, text)
        self.next_before = next_before if next_before is not None else _get_number_next(scene)
        item_number = int(getattr(item, "number", self.next_before or 1))
        default_after = max(int(self.next_before or 1), item_number) + 1
        self.next_after = next_after if next_after is not None else default_after

    def undo(self):
        super().undo()
        _set_number_next(self.scene, self.next_before)

    def redo(self):
        super().redo()
        _set_number_next(self.scene, self.next_after)


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


class RemoveNumberCommand(RemoveItemCommand):
    """移除序号图元，但保持删除前的“下一个序号”不变。"""

    def __init__(self, scene: QGraphicsScene, item: QGraphicsItem, text: str = "Remove Number"):
        super().__init__(scene, item, text)

    def undo(self):
        next_number = _get_number_next(self.scene)
        super().undo()
        _set_number_next(self.scene, next_number)

    def redo(self):
        next_number = _get_number_next(self.scene)
        super().redo()
        _set_number_next(self.scene, next_number)


class RemoveNumberAndRenumberCommand(RemoveItemCommand):
    """移除序号图元，并按当前数字和创建顺序重排剩余序号。"""

    def __init__(self, scene: QGraphicsScene, item: QGraphicsItem, text: str = "Remove Number And Renumber"):
        super().__init__(scene, item, text)
        self.next_before = _get_number_next(scene)
        self.old_numbers = {
            number_item: _number_value(number_item)
            for number_item in _get_number_items(scene)
        }
        remaining = [
            number_item
            for number_item in _get_number_items(scene)
            if number_item is not item
        ]
        self._renumber_start = min((_number_value(it) for it in remaining), default=1)
        self.new_numbers = self._build_new_numbers(remaining, self._renumber_start)
        self.next_after = self._renumber_start + len(remaining) if remaining else 1

    def _build_new_numbers(self, remaining: list, start: int) -> Dict[QGraphicsItem, int]:
        draw_order = _get_draw_order_map(self.scene)
        remaining.sort(
            key=lambda number_item: (
                _number_value(number_item),
                _number_order_value(number_item, draw_order.get(id(number_item), 0)),
            )
        )
        return {
            number_item: start + index
            for index, number_item in enumerate(remaining)
        }

    def undo(self):
        if self.item is not None and self.item.scene() != self.scene:
            self.scene.addItem(self.item)
        _apply_number_values(self.old_numbers)
        _set_number_next(self.scene, self.next_before)
        if self.scene is not None:
            self.scene.update()

    def redo(self):
        if self.item is not None and self.item.scene() == self.scene:
            self.scene.removeItem(self.item)
        _apply_number_values(self.new_numbers)
        _set_number_next(self.scene, self.next_after)
        if self.scene is not None:
            self.scene.update()


RemoveNumberItemCommand = RemoveNumberCommand


class BatchRemoveCommand(QUndoCommand):
    """批量移除图元命令（用于橡皮擦等工具）"""

    def __init__(self, scene: QGraphicsScene, items: list, text: str = "Remove Items", number_next_before: Optional[int] = None):
        super().__init__(text)
        self.scene = scene
        self.items = list(items)  # 复制列表避免外部修改
        self._has_number_items = _items_contain_number(self.items)
        self.number_next_before = number_next_before
        self._first_redo = True

    def undo(self):
        """撤销 - 恢复所有被删除的图元"""
        next_number = _get_number_next(self.scene) if self._has_number_items else None
        for item in self.items:
            if item is not None and item.scene() != self.scene:
                self.scene.addItem(item)
        _set_number_next(self.scene, next_number)

    def redo(self):
        """重做 - 删除所有图元"""
        if not self._has_number_items:
            next_number = None
        elif self._first_redo and self.number_next_before is not None:
            next_number = self.number_next_before
        else:
            next_number = _get_number_next(self.scene)

        for item in self.items:
            if item is not None and item.scene() == self.scene:
                self.scene.removeItem(item)
        _set_number_next(self.scene, next_number)
        self._first_redo = False


class NumberStyleCommand(QUndoCommand):
    """切换单个序号图元的样式。

    样式是会被矢量记录、随图元传递的属性，所以改它必须可撤销。
    """

    def __init__(self, item, old_style: str, new_style: str, text: str = None):
        super().__init__(text or _undo_tr("Change Number Style"))
        self.item = item
        self.old_style = old_style
        self.new_style = new_style

    def undo(self):
        self.item.set_style(self.old_style)

    def redo(self):
        self.item.set_style(self.new_style)


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

        point_size = state.get("font_point_size")
        if isinstance(point_size, (int, float)) and hasattr(
            self.item, "set_font_point_size"
        ):
            self.item.set_font_point_size(float(point_size))


        # opacity
        opacity = state.get("opacity")
        if isinstance(opacity, (int, float)) and hasattr(self.item, "setOpacity"):
            try:
                self.item.setOpacity(float(opacity))
            except Exception as e:
                log_exception(e, T("恢复opacity"))

        # number（NumberItem）
        number = state.get("number")
        if isinstance(number, int) and hasattr(self.item, "number"):
            try:
                self.item.number = max(1, int(number))
            except Exception as e:
                log_exception(e, T("恢复number属性"))

        # rect（RectItem/EllipseItem 等）
        rect = state.get("rect")
        if isinstance(rect, QRectF):
            if hasattr(self.item, "setRect") and callable(self.item.setRect):
                self.item.setRect(QRectF(rect))
            elif hasattr(self.item, "rect"):
                try:
                    self.item.rect = QRectF(rect)
                except Exception as e:
                    log_exception(e, T("恢复rect属性"))

        # start/end（ArrowItem / 自定义箭头）
        start = state.get("start")
        if isinstance(start, QPointF):
            # 兼容：item.start / item.start_pos
            if hasattr(self.item, "start"):
                try:
                    self.item.start = QPointF(start)
                except Exception as e:
                    log_exception(e, T("恢复start属性"))
            if hasattr(self.item, "start_pos"):
                try:
                    self.item.start_pos = QPointF(start)
                except Exception as e:
                    log_exception(e, T("恢复start_pos属性"))

        end = state.get("end")
        if isinstance(end, QPointF):
            # 兼容：item.end / item.end_pos
            if hasattr(self.item, "end"):
                try:
                    self.item.end = QPointF(end)
                except Exception as e:
                    log_exception(e, T("恢复end属性"))
            if hasattr(self.item, "end_pos"):
                try:
                    self.item.end_pos = QPointF(end)
                except Exception as e:
                    log_exception(e, T("恢复end_pos属性"))

        # 恢复箭头弯曲控制点和修改状态
        control = state.get("control")
        control_modified = state.get("control_modified")
        
        if hasattr(self.item, "_control_modified"):
            try:
                # 先恢复 _control_modified 状态
                if control_modified is not None:
                    self.item._control_modified = bool(control_modified)
                else:
                    # 向后兼容：如果没有 control_modified，根据 control 是否存在判断
                    self.item._control_modified = isinstance(control, QPointF)
                
                # 再恢复控制点位置
                if isinstance(control, QPointF):
                    self.item._control_pos = QPointF(control)
                elif hasattr(self.item, "start_pos") and hasattr(self.item, "end_pos"):
                    # 重置到中点
                    self.item._control_pos = QPointF(
                        (self.item.start_pos.x() + self.item.end_pos.x()) / 2,
                        (self.item.start_pos.y() + self.item.end_pos.y()) / 2
                    )
            except Exception as e:
                log_exception(e, T("恢复control状态"))
        elif hasattr(self.item, "set_control_point"):
            try:
                if isinstance(control, QPointF):
                    self.item.set_control_point(QPointF(control))
                elif hasattr(self.item, "reset_control_point"):
                    self.item.reset_control_point()
            except Exception as e:
                log_exception(e, T("恢复control_pos属性"))

        # 恢复箭头样式
        arrow_style = state.get("arrow_style")
        if arrow_style is not None and hasattr(self.item, "_arrow_style"):
            try:
                self.item._arrow_style = arrow_style
            except Exception as e:
                log_exception(e, T("恢复arrow_style属性"))
        
        # 恢复笔刷样式（画笔工具）
        pen_state = state.get("pen_state")
        if pen_state is not None and hasattr(self.item, "setPen"):
            try:
                from PySide6.QtGui import QPen
                from PySide6.QtCore import Qt
                pen = QPen()
                pen.setColor(pen_state.get('pen_color', Qt.GlobalColor.black))
                pen.setWidthF(pen_state.get('pen_width', 1.0))
                pen.setStyle(pen_state.get('pen_style', Qt.PenStyle.SolidLine))
                pen.setCapStyle(pen_state.get('pen_cap_style', Qt.PenCapStyle.RoundCap))
                pen.setJoinStyle(pen_state.get('pen_join_style', Qt.PenJoinStyle.RoundJoin))
                dash_pattern = pen_state.get('pen_dash_pattern', [])
                if dash_pattern:
                    pen.setDashPattern(dash_pattern)
                self.item.setPen(pen)
            except Exception as e:
                log_exception(e, T("恢复pen_state属性"))

        # 如果你的 item 有 update_geometry 之类的，顺便触发
        if hasattr(self.item, "update_geometry") and callable(self.item.update_geometry):
            try:
                self.item.update_geometry()
            except Exception as e:
                log_exception(e, "update_geometry")

        # 马赛克种类（框选马赛克：True=模糊，False=马赛克）
        smooth = state.get("smooth")
        if smooth is not None and hasattr(self.item, "set_smooth"):
            try:
                self.item.set_smooth(bool(smooth))
            except Exception as e:
                log_exception(e, T("恢复马赛克种类"))

        # 马赛克粒度（block_size 变了，配套的缩小图必须一起换成同一粒度那张）
        block_size = state.get("block_size")
        reduced_image = state.get("reduced_image")
        if block_size is not None and reduced_image is not None and hasattr(self.item, "set_block_size"):
            try:
                self.item.set_block_size(int(block_size), reduced_image)
            except Exception as e:
                log_exception(e, T("恢复马赛克粒度"))

        # 圆角半径（RectItem）
        corner_radius = state.get("corner_radius")
        if corner_radius is not None and hasattr(self.item, "set_corner_radius"):
            try:
                self.item.set_corner_radius(float(corner_radius))
            except Exception as e:
                log_exception(e, T("恢复corner_radius"))

        # 触发重绘
        if hasattr(self.item, "update"):
            self.item.update()


class NumberEditCommand(EditItemCommand):
    """序号加减命令：短时间连续点击同一图元时合并为一个撤销步骤。"""

    MERGE_WINDOW_SECONDS = 0.7
    COMMAND_ID = 0x4E554D  # "NUM"

    def __init__(
        self,
        item: QGraphicsItem,
        old_state: Dict[str, Any],
        new_state: Dict[str, Any],
        next_before: Optional[int] = None,
        next_after: Optional[int] = None,
        text: str = "Edit Number",
    ):
        super().__init__(item, old_state, new_state, text)
        self.scene = item.scene() if item is not None and hasattr(item, "scene") else None
        self.next_before = next_before
        self.next_after = next_after
        self._last_merge_time = time.monotonic()

    def undo(self):
        super().undo()
        _set_number_next(self.scene, self.next_before)

    def redo(self):
        super().redo()
        _set_number_next(self.scene, self.next_after)

    def id(self) -> int:
        return self.COMMAND_ID

    def mergeWith(self, other: QUndoCommand) -> bool:
        if not isinstance(other, NumberEditCommand):
            return False
        if self.item is None or self.item is not other.item:
            return False
        now = time.monotonic()
        if now - self._last_merge_time > self.MERGE_WINDOW_SECONDS:
            return False

        self.new_state = self._clone_state(other.new_state)
        self.next_after = other.next_after
        self._last_merge_time = now
        return True

 
