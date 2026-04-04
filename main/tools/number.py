"""
序号标注工具
"""

from PySide6.QtCore import QPointF, Qt
from .base import Tool, ToolContext, color_with_opacity
from canvas.items import NumberItem
from canvas.undo import AddItemCommand
from core import log_debug, log_warning
from core.logger import log_exception

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
    RADIUS_SCALE = 2

    @staticmethod
    def _count_numbers(scene) -> int:
        if not NumberTool._is_qobject_alive(scene):
            return 0
        try:
            return sum(1 for item in scene.items() if isinstance(item, NumberItem))
        except RuntimeError as exc:
            log_warning(f"scene.items() 失败：{exc}", "NumberTool")
            return 0
        except Exception as exc:
            log_warning(f"统计序号时异常：{exc}", "NumberTool")
            return 0

    @classmethod
    def get_radius_for_width(cls, stroke_width: float) -> float:
        base_width = max(1.0, float(stroke_width))
        return base_width * cls.RADIUS_SCALE

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
    
    def on_press(self, pos: QPointF, button, ctx: ToolContext):
        if button == Qt.MouseButton.LeftButton:
            # 动态计算序号（基于场景中已有的数量）
            number = self.get_next_number(ctx.scene)
            radius = self.get_radius_for_width(ctx.stroke_width)
            
            log_debug(f"创建前场景中序号数量: {number - 1}, 将创建序号: {number}", "NumberTool")
            
            item_color = color_with_opacity(ctx.color, ctx.opacity)
            item = NumberItem(number, pos, radius, item_color)
            
            # 提交到撤销栈（这会立即调用 redo()，将 item 添加到场景）
            command = AddItemCommand(ctx.scene, item)
            ctx.undo_stack.push(command)
            
            # 绘制完成后自动选择（方便调整）
            ctx.scene.item_auto_select_requested.emit(item)
            
            # 检查创建后的数量
            try:
                count_after = sum(1 for i in ctx.scene.items() if isinstance(i, NumberItem))
            except Exception as exc:
                count_after = "未知"
                log_warning(f"统计创建后序号失败：{exc}", "NumberTool")
            log_debug(f"创建后场景中序号数量: {count_after}", "NumberTool")
            
            # 通过信号更新光标（显示下一个序号）
            ctx.scene.cursor_tool_update_requested.emit("number", True)

            # 尝试更新工具栏的序号显示
            try:
                window = ctx.scene.views()[0].window() if ctx.scene.views() else None
                toolbar = getattr(window, "toolbar", None) if window else None
                if toolbar and hasattr(toolbar, "set_number_next_value"):
                    toolbar.set_number_next_value(self.get_next_number(ctx.scene))
            except Exception as e:
                log_exception(e, "更新工具栏序号显示")
    
    def _update_cursor(self, scene):
        """更新光标显示下一个序号"""
        if not self._is_qobject_alive(scene):
            log_debug("scene 已失效，跳过光标更新", "NumberTool")
            return
        next_num = self.get_next_number(scene)
        log_debug(f"更新光标时下一个序号: {next_num}", "NumberTool")
        
        view = getattr(scene, 'view', None)
        cursor_manager = getattr(view, 'cursor_manager', None) if view else None
        if self._is_qobject_alive(view) and self._is_qobject_alive(cursor_manager):
            try:
                cursor_manager.set_tool_cursor(self.id, force=True)
            except RuntimeError as exc:
                log_warning(f"设置光标失败：{exc}", "NumberTool")

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
 