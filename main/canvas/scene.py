"""
画布场景 - 管理所有图层和绘图工具
"""

from PySide6.QtWidgets import QGraphicsScene
from PySide6.QtCore import Signal, Qt
from PySide6.QtGui import QColor

from .items import BackgroundItem, SelectionItem
from .selection_model import SelectionModel
from .undo import CommandUndoStack
from tools import ToolController, ToolContext
from tools import (
    PenTool, RectTool, EllipseTool, ArrowTool,
    TextTool, NumberTool, HighlighterTool, CursorTool, EraserTool
)
from settings import get_tool_settings_manager
from core import log_debug


class CanvasScene(QGraphicsScene):
    """
    画布场景
    """
    
    selectionConfirmed = Signal()  # 选区确认信号
    # 解耦信号：Scene 通过信号通知 View，而不是直接持有 view 引用
    cursor_color_update_requested = Signal(object)    # 参数：QColor
    cursor_tool_update_requested = Signal(str, bool)  # 参数：tool_id, force
    item_auto_select_requested = Signal(object)       # 参数：绘制完成的 QGraphicsItem
    editing_cleanup_requested = Signal()              # 请求清除编辑状态（控制点、画笔指示器）
    
    def __init__(self, background_image, scene_rect):
        """
        Args:
            background_image: QImage - 背景图像
            scene_rect: QRectF - 场景坐标范围
        """
        super().__init__()
        
        from PySide6.QtCore import QRectF
        self.scene_rect = QRectF(scene_rect)
        
        # 先创建选区模型
        self.selection_model = SelectionModel()
        
        # 创建图层（传入model）
        self.background = BackgroundItem(background_image, self.scene_rect)
        self.selection_item = SelectionItem(self.selection_model)
        
        # Z-Order:
        # 0: Background
        # 10: Highlighter Items (由工具创建)
        # 20: Normal Drawing Items (由工具创建)
        # 101: Selection Item
        # (遮罩层已移至 ScreenshotWindow 的 QWidget 叠层 MaskOverlayWidget)
        
        self.background.setZValue(0)
        self.selection_item.setZValue(101)
        
        self.addItem(self.background)
        self.addItem(self.selection_item)
        
        # 设置场景范围 - 使用传入的 scene_rect，因为 BackgroundItem 使用了 setOffset
        # background.boundingRect() 返回的是 pixmap 本地坐标 (0,0,w,h)，不包含 offset
        self.setSceneRect(self.scene_rect)
        
        log_debug(f"场景创建完成: scene_rect={self.scene_rect}, sceneRect()={self.sceneRect()}", "CanvasScene")
        
        # 撤销栈 (命令模式)
        self.undo_stack = CommandUndoStack(self)
        
        # 连接撤销栈信号，用于在撤销/重做后更新序号光标
        self.undo_stack.indexChanged.connect(self._on_undo_stack_changed)
        
        # 获取工具设置管理器
        self.tool_settings_manager = get_tool_settings_manager()
        
        # 从设置管理器读取默认工具(pen)的初始设置，而不是硬编码
        initial_color = self.tool_settings_manager.get_color("pen")
        initial_stroke_width = self.tool_settings_manager.get_stroke_width("pen")
        initial_opacity = self.tool_settings_manager.get_opacity("pen")
        
        # 工具控制器
        ctx = ToolContext(
            scene=self,
            selection=self.selection_model,
            undo_stack=self.undo_stack,
            color=initial_color,
            stroke_width=initial_stroke_width,
            opacity=initial_opacity,
            settings_manager=self.tool_settings_manager
        )
        self.tool_controller = ToolController(ctx)
        self.tool_controller.add_tool_changed_callback(self.on_tool_changed)
        
        # LayerEditor 实例，由 View 在连接信号时注入
        self._layer_editor = None
        
        # 注册工具
        self.tool_controller.register(CursorTool())  # 光标工具（默认/无绘制工具，实际选择编辑由SmartEditController处理）
        self.tool_controller.register(PenTool())
        self.tool_controller.register(RectTool())
        self.tool_controller.register(EllipseTool())
        self.tool_controller.register(ArrowTool())
        self.tool_controller.register(TextTool())
        self.tool_controller.register(NumberTool())
        self.tool_controller.register(HighlighterTool())
        self.tool_controller.register(EraserTool())  # 橡皮擦工具
        
        # 默认激活光标工具（表示无绘制工具激活，SmartEditController负责选择/编辑交互）
        self.tool_controller.activate("cursor")
    
    def confirm_selection(self):
        """
        确认选区
        """
        self.selection_model.confirm()
        
        # 确认选区后，选区框应该保持显示，以便调整
        # 只有在开始绘图时，或者用户明确想要隐藏时才隐藏
        # 但根据用户反馈"选区后没有可以调整的框"，说明这里不应该隐藏
        self.selection_item.show()
        
        self.selectionConfirmed.emit()

    
    def activate_tool(self, tool_id: str):
        """
        激活工具
        """
        self.tool_controller.activate(tool_id)
    
    def update_style(self, **kwargs):
        """
        更新样式
        """
        self.tool_controller.update_style(**kwargs)

        # 颜色变化通过信号通知 View 更新光标
        if 'color' in kwargs:
            color = kwargs.get('color')
            if color is not None:
                self.cursor_color_update_requested.emit(color)
    
    def on_tool_changed(self, tool_id):
        """
        工具切换回调 - 通过信号通知 View 更新光标
        """
        self.cursor_tool_update_requested.emit(tool_id, False)
    
    def _on_undo_stack_changed(self):
        """
        撤销栈变化时的回调（用于更新序号工具光标）
        """
        if hasattr(self, 'tool_controller') and self.tool_controller and self.tool_controller.current_tool:
            if self.tool_controller.current_tool.id == "number":
                self.cursor_tool_update_requested.emit("number", True)
    
    def drawForeground(self, painter, rect):
        """
        绘制前景层 - 渲染 LayerEditor 控制点
        """
        super().drawForeground(painter, rect)
        
        # 渲染智能编辑控制点（通过注入的 layer_editor 引用，无需持有 view）
        if self._layer_editor and self._layer_editor.is_editing():
            self._layer_editor.render(painter)
    
    def get_drawing_items_in_rect(self, rect):
        """
        获取选区内的所有绘制项目（按绘制顺序）
        
        Args:
            rect: 选区矩形（场景坐标，QRectF）
            
        Returns:
            list: 选区内的绘制项目列表（按绘制顺序，先绘制的在前）
        """
        from PySide6.QtCore import Qt
        from PySide6.QtWidgets import QGraphicsEllipseItem
        
        drawing_items = []
        excluded_items = {self.background, self.selection_item}
        
        # 使用场景的 items() 方法查找矩形范围内的项目
        # IntersectsItemBoundingRect: 只要边界框相交就算
        items_in_rect = self.items(rect, Qt.ItemSelectionMode.IntersectsItemBoundingRect)
        
        for item in items_in_rect:
            # 排除基础UI元素
            if item in excluded_items:
                continue
            
            # 排除画笔指示器（Z值=10000的QGraphicsEllipseItem）
            if isinstance(item, QGraphicsEllipseItem) and item.zValue() >= 10000:
                continue
            
            drawing_items.append(item)
        
        # 反转列表，使其按绘制顺序（先绘制的在前）
        # scene.items() 返回的是Z-order排序（上层在前），但我们需要绘制顺序
        drawing_items.reverse()
        
        log_debug(f"选区内绘制项目: {len(drawing_items)} 个（已按绘制顺序排列）", "Scene")
        return drawing_items
 