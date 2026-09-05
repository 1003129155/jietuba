"""
序号标注工具
"""

from PySide6.QtCore import QPointF, Qt
from .base import Tool, ToolContext, color_with_opacity
from canvas.items import NumberItem
from canvas.undo import AddNumberCommand
from core import log_debug, log_warning
from core.logger import log_exception, T

try:
    import shiboken6  as _shiboken
except Exception:
    _shiboken = None


class NumberTool(Tool):
    """
    序号标注工具
    """
    
    id = "number"
    _SCENE_OFFSET_ATTR = "_number_tool_offset"
    _SCENE_ORDER_ATTR = "_number_tool_order_counter"
    RADIUS_SCALE = 2
    # 序号的合法字号范围。工具栏把同一个笔触宽度广播给所有面板，画笔可以细到
    # 1，序号不行——低于 MIN_WIDTH 的圈小到看不清数字。面板按这个范围钳制显示，
    # 所以算半径也必须用同一个范围，否则会出现"面板显示 8、画出来却是 1"。
    MIN_WIDTH = 8
    MAX_WIDTH = 72

    @staticmethod
    def _count_numbers(scene) -> int:
        if not NumberTool._is_qobject_alive(scene):
            return 0
        try:
            return sum(1 for item in scene.items() if isinstance(item, NumberItem))
        except RuntimeError as exc:
            log_warning(T("scene.items() 失败：{exc}", exc=exc), "NumberTool")
            return 0
        except Exception as exc:
            log_warning(T("统计序号时异常：{exc}", exc=exc), "NumberTool")
            return 0

    @staticmethod
    def get_max_number(scene, override_item=None, override_number=None) -> int:
        """获取场景中最大的序号值，没有序号时返回 0。"""
        if not NumberTool._is_qobject_alive(scene):
            return 0
        try:
            max_number = 0
            for item in scene.items():
                if isinstance(item, NumberItem):
                    if override_item is not None and item is override_item:
                        number = int(override_number)
                    else:
                        number = int(getattr(item, "number", 0))
                    max_number = max(max_number, number)
            return max_number
        except RuntimeError as exc:
            log_warning(T("scene.items() 失败：{exc}", exc=exc), "NumberTool")
            return 0
        except Exception as exc:
            log_warning(T("统计最大序号时异常：{exc}", exc=exc), "NumberTool")
            return 0

    @classmethod
    def clamp_width(cls, stroke_width: float) -> float:
        """把共享的笔触宽度收进序号自己的合法范围。"""
        try:
            width = float(stroke_width)
        except (TypeError, ValueError):
            width = cls.MIN_WIDTH
        return max(float(cls.MIN_WIDTH), min(float(cls.MAX_WIDTH), width))

    @classmethod
    def get_radius_for_width(cls, stroke_width: float) -> float:
        return cls.clamp_width(stroke_width) * cls.RADIUS_SCALE

    @classmethod
    def get_next_number(cls, scene) -> int:
        """
        获取下一个序号数字（基于场景中已有的序号数量 + 偏移量）
        """
        if not cls._is_qobject_alive(scene):
            return 1

        base_count = cls._count_numbers(scene)
        try:
            offset = getattr(scene, cls._SCENE_OFFSET_ATTR, 0)
        except RuntimeError:
            return 1
        next_number = base_count + 1 + offset
        return max(1, next_number)

    @classmethod
    def adjust_next_number(cls, scene, step: int) -> int:
        """根据滚轮方向调整下一次使用的序号"""
        if not cls._is_qobject_alive(scene) or step == 0:
            return cls.get_next_number(scene)

        base_count = cls._count_numbers(scene)
        try:
            offset = getattr(scene, cls._SCENE_OFFSET_ATTR, 0) + step
        except RuntimeError:
            return cls.get_next_number(scene)
        # 确保序号至少为 1
        min_offset = 1 - (base_count + 1)
        offset = max(min_offset, offset)
        setattr(scene, cls._SCENE_OFFSET_ATTR, offset)
        next_number = base_count + 1 + offset
        return max(1, next_number)

    @classmethod
    def set_next_number(cls, scene, next_number: int) -> int:
        """直接设置下一次使用的序号"""
        if not cls._is_qobject_alive(scene):
            return 1
        base_count = cls._count_numbers(scene)
        try:
            next_number = max(1, int(next_number))
        except Exception:
            next_number = base_count + 1
        offset = next_number - (base_count + 1)
        min_offset = 1 - (base_count + 1)
        offset = max(min_offset, offset)
        try:
            setattr(scene, cls._SCENE_OFFSET_ATTR, offset)
        except RuntimeError:
            return cls.get_next_number(scene)
        return max(1, base_count + 1 + offset)

    @classmethod
    def set_next_number_and_refresh(cls, scene, next_number: int, force_cursor: bool = True) -> int:
        """设置下一序号，并统一刷新工具栏与光标。"""
        actual_next = cls.set_next_number(scene, next_number)
        cls.refresh_next_number(scene, actual_next, force_cursor=force_cursor)
        return actual_next

    @classmethod
    def refresh_next_number(cls, scene, next_number: int | None = None, force_cursor: bool = True) -> int:
        """刷新序号工具栏和光标预览，返回当前显示的下一序号。"""
        if next_number is None:
            next_number = cls.get_next_number(scene)
        try:
            views = scene.views() if cls._is_qobject_alive(scene) and hasattr(scene, "views") else []
            view = views[0] if views else None
            window = view.window() if view is not None else None
            toolbar = getattr(window, "toolbar", None) if window is not None else None
            if toolbar and hasattr(toolbar, "set_number_next_value"):
                toolbar.set_number_next_value(int(next_number))
            # 只有当前激活的是序号工具时才更新光标，避免橡皮擦等工具删除序号图元时误切光标
            if force_cursor and hasattr(scene, "cursor_tool_update_requested"):
                tc = getattr(scene, "tool_controller", None)
                current_tool = getattr(tc, "current_tool", None) if tc else None
                if current_tool is not None and current_tool.id == cls.id:
                    scene.cursor_tool_update_requested.emit("number", True)
        except Exception as e:
            log_exception(e, T("刷新序号计数器"))
        return max(1, int(next_number))

    @classmethod
    def get_next_after_number_edit(cls, scene, item, old_number: int, new_number: int, current_next: int | None = None) -> int:
        """根据序号 +/- 结果计算下一序号。"""
        if current_next is None:
            current_next = cls.get_next_number(scene)
        current_next = max(1, int(current_next))
        old_number = max(1, int(old_number))
        new_number = max(1, int(new_number))

        if new_number >= current_next:
            return new_number + 1

        old_max = cls.get_max_number(scene)
        if old_number < old_max:
            return current_next

        new_max = cls.get_max_number(scene, override_item=item, override_number=new_number)
        return max(1, new_max + 1)

    @classmethod
    def assign_number_order(cls, scene, item) -> int:
        """给序号图元分配稳定创建顺序，用于重复数字时排序。"""
        if item is None:
            return 0

        existing = getattr(item, "number_order", None)
        if isinstance(existing, int) and existing >= 0:
            return existing

        if not cls._is_qobject_alive(scene):
            item.number_order = 0
            return 0

        try:
            counter = int(getattr(scene, cls._SCENE_ORDER_ATTR, 0))
        except Exception:
            counter = 0

        try:
            max_order = -1
            for scene_item in scene.items():
                if isinstance(scene_item, NumberItem):
                    order = getattr(scene_item, "number_order", None)
                    if isinstance(order, int):
                        max_order = max(max_order, order)
            counter = max(counter, max_order + 1)
        except Exception as exc:
            log_warning(T("同步序号创建顺序失败：{exc}", exc=exc), "NumberTool")

        item.number_order = counter
        try:
            setattr(scene, cls._SCENE_ORDER_ATTR, counter + 1)
        except RuntimeError:
            pass
        return counter
    
    def on_press(self, pos: QPointF, button, ctx: ToolContext):
        if button == Qt.MouseButton.LeftButton:
            # 动态计算序号（基于场景中已有的数量）
            number = self.get_next_number(ctx.scene)
            radius = self.get_radius_for_width(ctx.stroke_width)
            
            log_debug(T("创建前场景中序号数量: {prev_count}, 将创建序号: {number}", prev_count=number - 1, number=number), "NumberTool")
            
            item_color = color_with_opacity(ctx.color, ctx.opacity)
            item = NumberItem(number, pos, radius, item_color)
            self.assign_number_order(ctx.scene, item)
            
            # 提交到撤销栈（这会立即调用 redo()，将 item 添加到场景）
            command = AddNumberCommand(ctx.scene, item, next_before=number)
            ctx.undo_stack.push(command)
            
            # 绘制完成后自动选择（方便调整）
            ctx.scene.item_auto_select_requested.emit(item)
            
            # 检查创建后的数量
            try:
                count_after = sum(1 for i in ctx.scene.items() if isinstance(i, NumberItem))
            except Exception as exc:
                count_after = "未知"
                log_warning(T("统计创建后序号失败：{exc}", exc=exc), "NumberTool")
            log_debug(T("创建后场景中序号数量: {count_after}", count_after=count_after), "NumberTool")
            
    
    def _update_cursor(self, scene):
        """更新光标显示下一个序号"""
        if not self._is_qobject_alive(scene):
            log_debug(T("scene 已失效，跳过光标更新"), "NumberTool")
            return
        next_num = self.get_next_number(scene)
        log_debug(T("更新光标时下一个序号: {next_num}", next_num=next_num), "NumberTool")
        
        view = getattr(scene, 'view', None)
        cursor_manager = getattr(view, 'cursor_manager', None) if view else None
        if self._is_qobject_alive(view) and self._is_qobject_alive(cursor_manager):
            try:
                cursor_manager.set_tool_cursor(self.id, force=True)
            except RuntimeError as exc:
                log_warning(T("设置光标失败：{exc}", exc=exc), "NumberTool")

    @staticmethod
    def _is_qobject_alive(obj) -> bool:
        if obj is None:
            return False
        if _shiboken is None:
            return True
        try:
            return _shiboken.isValid(obj)
        except Exception:
            return True
 
