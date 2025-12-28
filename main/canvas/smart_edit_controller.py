"""
智能编辑控制器 - Smart Edit Controller
基于 QGraphicsItem 架构，提供智能选择和编辑功能

功能:
1. 智能选择 - 根据工具类型和图元类型判断是否可选
2. 悬停检测 - 显示十字光标和高亮
3. 编辑控制点 - 集成 LayerEditor 显示控制点
4. 拖拽优先级 - 处理选区/钉图移动和内容编辑的优先级
"""

from enum import Enum
from typing import Optional, Dict, Any
from PyQt6.QtCore import QObject, pyqtSignal, QPointF, Qt
from PyQt6.QtWidgets import QGraphicsItem, QGraphicsScene

from canvas.items import StrokeItem, RectItem, EllipseItem, ArrowItem, TextItem, NumberItem
from canvas.layer_editor import LayerEditor
from canvas.undo import EditItemCommand


# ============================================================================
#  选择模式枚举
# ============================================================================

class SelectionMode(Enum):
    """选择模式"""
    NONE = "none"                    # 无选择
    HOVER = "hover"                  # 悬停（显示十字光标）
    SELECTED = "selected"            # 已选中（显示控制点）
    EDITING = "editing"              # 编辑中（拖拽控制点）
    DRAGGING_MOVE = "dragging_move"  # 拖拽移动图元
    DRAGGING_HANDLE = "dragging_handle"  # 拖拽控制点


# ============================================================================
#  图元类型枚举
# ============================================================================

class ItemType(Enum):
    """图元类型"""
    PATH = "path"          # 画笔/荧光笔路径
    SHAPE = "shape"        # 形状（矩形、椭圆）
    ARROW = "arrow"        # 箭头
    TEXT = "text"          # 文字
    NUMBER = "number"      # 序号
    OTHER = "other"        # 其他


# ============================================================================
#  智能编辑控制器
# ============================================================================

class SmartEditController(QObject):
    """
    智能编辑控制器
    
    核心逻辑:
    1. 画笔/荧光笔(StrokeItem): 必须 Ctrl+点击才能选择，无悬停十字光标
    2. 形状/箭头(RectItem/ArrowItem等): 悬停显示十字光标，直接点击选择
    3. 无工具模式: 点击选择内容，拖拽移动选区/钉图窗口
    4. 有工具模式: 只有匹配类型的图元才能选择和编辑
    """
    
    # 信号：选择改变
    selection_changed = pyqtSignal(object)  # 参数：被选中的 QGraphicsItem 或 None
    
    # 信号：悬停改变
    hover_changed = pyqtSignal(object)      # 参数：悬停的 QGraphicsItem 或 None
    
    # 信号：光标改变请求
    cursor_change_request = pyqtSignal(str)  # 参数：光标类型 ("cross", "default", "move", "resize")
    
    def __init__(self, scene: QGraphicsScene):
        """
        Args:
            scene: QGraphicsScene 对象
        """
        super().__init__()
        
        self.scene = scene
        
        # 连接撤销栈信号（如果存在），用于撤销时同步手柄
        if hasattr(self.scene, "undo_stack"):
            self.scene.undo_stack.indexChanged.connect(self._on_undo_index_changed)
        
        # 当前状态
        self.mode = SelectionMode.NONE
        self.selected_item: Optional[QGraphicsItem] = None
        self.hovered_item: Optional[QGraphicsItem] = None
        
        # 当前工具 ID
        self.current_tool_id: Optional[str] = None
        
        # 拖拽状态
        self.drag_start_pos: Optional[QPointF] = None
        self.drag_threshold = 5.0  # 5像素拖拽阈值
        self.is_dragging = False
        
        # 编辑器（控制点系统）
        self.layer_editor = LayerEditor()  # LayerEditor 实例
        self._move_initial_state = None
        
        # 标记是否刚刚清除了选择（用于阻止立即绘图）
        self._just_cleared_selection = False
        
        # 标记是否是自动选择（绘制后自动选中）
        self._is_auto_selected = False
    
    # ========================================================================
    #  工具状态管理
    # ========================================================================
    
    def set_tool(self, tool_id: Optional[str]):
        """
        设置当前工具
        
        Args:
            tool_id: 工具 ID (pen, rect, arrow, etc.) 或 None (无工具/光标工具)
        """
        self.current_tool_id = tool_id
        
        # 切换工具时清除选择
        if self.selected_item:
            # 工具切换导致的清除不应该阻止下一次绘图
            self.clear_selection(suppress_block=True)
    
    # ========================================================================
    #  图元类型判定
    # ========================================================================
    
    def get_item_type(self, item: QGraphicsItem) -> ItemType:
        """
        判定图元类型
        
        Args:
            item: QGraphicsItem 对象
            
        Returns:
            ItemType 枚举
        """
        if not item:
            return ItemType.OTHER
        
        # 使用 isinstance 判断类型
        if isinstance(item, StrokeItem):
            return ItemType.PATH
        elif isinstance(item, (RectItem, EllipseItem)):
            return ItemType.SHAPE
        elif isinstance(item, ArrowItem):
            return ItemType.ARROW
        elif isinstance(item, TextItem):
            return ItemType.TEXT
        elif isinstance(item, NumberItem):
            return ItemType.NUMBER
        else:
            return ItemType.OTHER
    
    # ========================================================================
    #  选择逻辑
    # ========================================================================
    
    def can_select_item(self, item: QGraphicsItem, modifier_keys: int) -> bool:
        """
        判断是否可以选择该图元
        
        Args:
            item: QGraphicsItem 对象
            modifier_keys: 修饰键状态 (Qt.KeyboardModifier)
            
        Returns:
            bool - 是否可以选择
        """
        from PyQt6.QtCore import Qt
        
        item_type = self.get_item_type(item)
        
        # 1. 画笔/荧光笔路径：必须 Ctrl+点击
        if item_type == ItemType.PATH:
            return bool(modifier_keys & Qt.KeyboardModifier.ControlModifier)
        
        # 2. 无工具模式（光标工具）：禁止选择绘制图元（优先移动选区）
        if not self.current_tool_id or self.current_tool_id == "cursor":
            return False
        
        # 3. 有工具模式：只能选择匹配类型的图元
        tool_to_type = {
            "pen": ItemType.PATH,
            "highlighter": ItemType.PATH,
            "rect": ItemType.SHAPE,
            "ellipse": ItemType.SHAPE,
            "arrow": ItemType.ARROW,
            "text": ItemType.TEXT,
            "number": ItemType.NUMBER,
        }
        
        expected_type = tool_to_type.get(self.current_tool_id)
        return item_type == expected_type
    
    def can_show_hover_cursor(self, item: QGraphicsItem) -> bool:
        """
        判断是否显示悬停十字光标
        
        Args:
            item: QGraphicsItem 对象
            
        Returns:
            return
        """
        item_type = self.get_item_type(item)
        
        # 画笔/荧光笔路径：不显示十字光标（需要 Ctrl 才能交互）
        if item_type == ItemType.PATH:
            return False
        
        # 无工具模式（光标工具）：不显示十字光标（优先移动选区）
        if not self.current_tool_id or self.current_tool_id == "cursor":
            return False
        
        # 工具激活时，只对匹配类型显示光标
        tool_to_type = {
            "rect": ItemType.SHAPE,
            "ellipse": ItemType.SHAPE,
            "arrow": ItemType.ARROW,
            "text": ItemType.TEXT,
            "number": ItemType.NUMBER,
        }
        
        expected_type = tool_to_type.get(self.current_tool_id)
        return item_type == expected_type
    
    # ========================================================================
    #  鼠标事件处理
    # ========================================================================
    
    def handle_hover(self, pos: QPointF, scene_pos: QPointF) -> bool:
        """
        处理悬停事件
        
        Args:
            pos: 视图坐标
            scene_pos: 场景坐标
            
        Returns:
            bool - 是否悬停在可编辑图元上
        """
        items = self.scene.items(scene_pos)
        drawable_items = [
            item for item in items
            if isinstance(item, (StrokeItem, RectItem, EllipseItem, ArrowItem, TextItem, NumberItem))
        ]

        if drawable_items:
            item = drawable_items[0]
            if self.can_show_hover_cursor(item):
                if self.hovered_item != item:
                    self.hovered_item = item
                    self.hover_changed.emit(item)
                    # 不再发射 cursor_change_request 信号，由 view.py 直接设置光标
                return True
            else:
                if self.hovered_item:
                    self.hovered_item = None
                    self.hover_changed.emit(None)
                return False

        if self.hovered_item:
            self.hovered_item = None
            self.hover_changed.emit(None)

        return False
    
    def handle_press(self, pos: QPointF, scene_pos: QPointF, button: int, modifiers: int) -> bool:
        """
        处理鼠标按下事件
        
        Args:
            pos: 视图坐标
            scene_pos: 场景坐标
            button: 鼠标按钮
            modifiers: 修饰键状态
            
        Returns:
            bool - 是否选中了图元（True=拦截绘图，False=允许绘图）
        """
        from PyQt6.QtCore import Qt
        
        if button != Qt.MouseButton.LeftButton:
            return False
        
        # 记录拖拽起点
        self.drag_start_pos = scene_pos
        self.is_dragging = False
        
        # 获取点击的图元
        items = self.scene.items(scene_pos)
        drawable_items = [
            item for item in items
            if isinstance(item, (StrokeItem, RectItem, EllipseItem, ArrowItem, TextItem, NumberItem))
        ]
        
        if drawable_items:
            item = drawable_items[0]
            
            # 如果点击的是已经选中的图元，直接返回 True（允许拖拽/编辑）
            if self.selected_item == item:
                return True
            
            # 检查是否可以选择这个新图元
            if self.can_select_item(item, modifiers):
                # 选择图元
                self.select_item(item)
                # 返回 True，表示选中了图元，阻止绘图
                return True
        else:
            # 点击空白处，取消选择
            if self.selected_item:
                self.clear_selection()
        
        # 返回 False，表示未选中图元，允许绘图或其他操作
        return False
    
    def handle_move(self, pos: QPointF, scene_pos: QPointF) -> bool:
        """
        处理鼠标移动事件
        
        Args:
            pos: 视图坐标
            scene_pos: 场景坐标
            
        Returns:
            bool - 是否处理了事件
        """
        # 检查是否开始拖拽
        if self.drag_start_pos and not self.is_dragging:
            delta = scene_pos - self.drag_start_pos
            distance = (delta.x() ** 2 + delta.y() ** 2) ** 0.5
            
            if distance > self.drag_threshold:
                self.is_dragging = True
                
                # 如果拖拽的是选中的图元，进入移动模式
                if self.selected_item:
                    self.mode = SelectionMode.DRAGGING_MOVE
                    if self._move_initial_state is None:
                        self._move_initial_state = self._capture_layer_state(self.selected_item)
        
        # 如果正在拖拽选中的图元
        if self.is_dragging and self.selected_item:
            if self._move_initial_state is None:
                self._move_initial_state = self._capture_layer_state(self.selected_item)
            # 这里可以添加实时移动逻辑
            # 但为了不干扰 QGraphicsItem 自带的移动，暂时不处理
            pass
        
        return False
    
    def handle_release(self, pos: QPointF, scene_pos: QPointF, button: int) -> bool:
        """
        处理鼠标释放事件
        
        Args:
            pos: 视图坐标
            scene_pos: 场景坐标
            button: 鼠标按钮
            
        Returns:
            bool - 是否处理了事件
        """
        from PyQt6.QtCore import Qt
        
        if button != Qt.MouseButton.LeftButton:
            return False
        
        # 重置拖拽状态
        if self.is_dragging:
            self.is_dragging = False
            
            # 如果是移动模式，回到选中模式
            if self.mode == SelectionMode.DRAGGING_MOVE:
                self._finalize_move_edit()
                self.mode = SelectionMode.SELECTED
        
        self.drag_start_pos = None
        if self.mode != SelectionMode.DRAGGING_MOVE:
            self._move_initial_state = None
        
        return False
    
    # ========================================================================
    #  选择管理
    # ========================================================================
    
    def select_item(self, item: QGraphicsItem, auto_select: bool = False):
        """
        选择图元
        
        Args:
            item: 要选择的 QGraphicsItem
            auto_select: 是否是自动选择（绘制后自动选中）
        """
        if self.selected_item == item:
            return
        
        # 取消之前的选择
        if self.selected_item:
            self.selected_item.setSelected(False)
        
        # 选择新图元
        self.selected_item = item
        item.setSelected(True)
        self.mode = SelectionMode.SELECTED
        self._move_initial_state = None
        self._is_auto_selected = auto_select  # 记录是否是自动选择
        
        # 发送信号
        self.selection_changed.emit(item)
        
        # 显示编辑控制点
        if self.layer_editor:
            if isinstance(item, TextItem):
                # 文字图元使用自身编辑体验，禁用 LayerEditor 控制点
                self.layer_editor.stop_edit()
            else:
                self.layer_editor.start_edit(item)
                # 触发场景重绘，显示控制点
                self.scene.update()
    
    def clear_selection(self, suppress_block: bool = False):
        """清除选择"""
        if self.selected_item:
            current_item = self.selected_item
            self.selected_item.setSelected(False)
            
            # 如果是自动选择（绘制后自动选中），清除时不阻止下次绘图
            was_auto_selected = self._is_auto_selected
            
            self.selected_item = None
            self.mode = SelectionMode.NONE
            self._move_initial_state = None
            self._is_auto_selected = False
            
            
            # 只有手动选择后取消，才阻止下次点击绘图
            # 自动选择后取消，允许立即绘图（支持连续绘制）
            if not was_auto_selected and not suppress_block:
                self._just_cleared_selection = True
            
            # 发送信号
            self.selection_changed.emit(None)
            
            # 隐藏编辑控制点
            if self.layer_editor:
                self.layer_editor.stop_edit()
                # 触发场景重绘，隐藏控制点
                self.scene.update()


    def _is_text_item_editing(self, item: QGraphicsItem) -> bool:
        if not isinstance(item, TextItem):
            return False
        flags = item.textInteractionFlags()
        return bool(flags & Qt.TextInteractionFlag.TextEditorInteraction)

    # ========================================================================
    #  控制点编辑集成
    # ========================================================================

    def handle_edit_press(self, scene_pos: QPointF, view_pos: QPointF, button: int, modifiers: int):
        """在选中状态下处理控制点按下，返回是否拦截"""
        from PyQt6.QtCore import Qt
        if button != Qt.MouseButton.LeftButton:
            return False
        if not self.selected_item or not self.layer_editor:
            return False

        hit = self.layer_editor.hit_test(scene_pos)
        if not hit:
            return False

        self.mode = SelectionMode.DRAGGING_HANDLE
        keep_ratio = bool(modifiers & Qt.KeyboardModifier.ShiftModifier)
        self.layer_editor.start_drag(hit, scene_pos)
        self.keep_ratio = keep_ratio  # 临时记录
        return True

    def handle_edit_move(self, scene_pos: QPointF):
        if self.mode != SelectionMode.DRAGGING_HANDLE or not self.layer_editor:
            return False
            
        # 优化：只更新受影响的区域而不是全场景重绘
        # 1. 获取移动前的区域
        old_rect = self.layer_editor._get_scene_rect(self.selected_item)
        
        self.layer_editor.drag_to(scene_pos, keep_ratio=getattr(self, "keep_ratio", False))
        
        # 2. 获取移动后的区域
        new_rect = self.layer_editor._get_scene_rect(self.selected_item)
        
        # 3. 触发局部重绘
        if old_rect and new_rect:
            # 合并区域并外扩以包含手柄（手柄通常在边界外）
            margin = 25  # 手柄大小(10) + 额外缓冲
            update_rect = old_rect.united(new_rect).adjusted(-margin, -margin, margin, margin)
            self.scene.update(update_rect)
        else:
            # 降级方案
            self.scene.update()
            
        return True

    def handle_edit_release(self, scene_pos: QPointF, button: int):
        from PyQt6.QtCore import Qt
        if button != Qt.MouseButton.LeftButton:
            return False
        if self.mode != SelectionMode.DRAGGING_HANDLE or not self.layer_editor:
            return False

        # 推入撤销栈
        self.layer_editor.end_drag(getattr(self.scene, "undo_stack", None))
        self.mode = SelectionMode.SELECTED
        self.keep_ratio = False
        # 触发场景重绘
        self.scene.update()
        return True
    
    def get_selected_item(self) -> Optional[QGraphicsItem]:
        """获取当前选中的图元"""
        return self.selected_item

    def _capture_layer_state(self, item: Optional[QGraphicsItem]) -> Optional[Dict[str, Any]]:
        if not item or not self.layer_editor:
            return None
        if hasattr(self.layer_editor, "capture_state"):
            return self.layer_editor.capture_state(item)
        return None

    def _finalize_move_edit(self):
        if not self.layer_editor or not self.selected_item:
            self._move_initial_state = None
            return

        if self._move_initial_state is None:
            return

        new_state = self.layer_editor.capture_state(self.selected_item)
        
        if new_state == self._move_initial_state:
            self._move_initial_state = None
            return
        
        if not new_state:
            self._move_initial_state = None
            return

        undo_stack = getattr(self.scene, "undo_stack", None)
        if undo_stack and EditItemCommand:
            try:
                cmd = EditItemCommand(self.selected_item, self._move_initial_state, new_state)
                if hasattr(undo_stack, "push_command"):
                    undo_stack.push_command(cmd)
                else:
                    undo_stack.push(cmd)
            except Exception as exc:
                print(f"[SmartEdit] push move undo failed: {exc}")

        if isinstance(self.selected_item, TextItem):
            self.layer_editor.stop_edit()
        else:
            self.layer_editor.start_edit(self.selected_item)
        self.scene.update()
        self._move_initial_state = None
    
    # ========================================================================
    #  调试信息
    # ========================================================================
    
    def __repr__(self):
        return (
            f"SmartEditController(\n"
            f"  mode={self.mode.value},\n"
            f"  tool={self.current_tool_id},\n"
            f"  selected={type(self.selected_item).__name__ if self.selected_item else None},\n"
            f"  hovered={type(self.hovered_item).__name__ if self.hovered_item else None}\n"
            f")"
        )
    
    def _on_undo_index_changed(self):
        """撤销/重做发生时，更新控制点"""
        if self.selected_item:
            # 检查选中的图元是否还在场景中
            if self.selected_item.scene() is None:
                # 图元已被撤销（不在场景中了），清除选择
                self.selected_item = None
                self.mode = SelectionMode.NONE
                self._move_initial_state = None
                self.selection_changed.emit(None)
                if self.layer_editor:
                    self.layer_editor.stop_edit()
                if self.scene:
                    self.scene.update()
            elif self.layer_editor:
                # 图元还在，重新生成控制点以匹配新状态
                if isinstance(self.selected_item, TextItem):
                    self.layer_editor.stop_edit()
                else:
                    self.layer_editor.start_edit(self.selected_item)
                self.scene.update()

    # ========================================================================
    #  文字属性更新槽函数
    # ========================================================================

    def on_text_font_changed(self, font):
        """更新选中文字的字体"""
        if self.selected_item and isinstance(self.selected_item, TextItem):
            # TODO: 支持撤销
            self.selected_item.setFont(font)
            self.selected_item.update()
            # 文字图元不使用 LayerEditor 控制点，若此前被激活则关闭
            if self.layer_editor:
                self.layer_editor.stop_edit()

    def on_text_color_changed(self, color):
        """更新选中文字的颜色"""
        if self.selected_item and isinstance(self.selected_item, TextItem):
            self.selected_item.setDefaultTextColor(color)
            self.selected_item.update()

    def on_text_outline_changed(self, enabled, color, width):
        """更新选中文字的描边"""
        if self.selected_item and isinstance(self.selected_item, TextItem):
            self.selected_item.set_outline(enabled, color, width)
            self.selected_item.update()

    def on_text_shadow_changed(self, enabled, color):
        """更新选中文字的阴影"""
        if self.selected_item and isinstance(self.selected_item, TextItem):
            self.selected_item.set_shadow(enabled, color)
            self.selected_item.update()

    def on_text_background_changed(self, enabled, color):
        """更新选中文字的背景"""
        if self.selected_item and isinstance(self.selected_item, TextItem):
            self.selected_item.set_background(enabled, color)
            self.selected_item.update()
