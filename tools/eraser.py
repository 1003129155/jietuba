"""
橡皮擦工具 - 粗暴删除法
点击或拖动删除整个 QGraphicsItem
"""

from PyQt6.QtCore import QPointF, Qt
from PyQt6.QtGui import QPainterPath, QPainterPathStroker
from .base import Tool, ToolContext
from canvas.items import StrokeItem, RectItem, EllipseItem, ArrowItem, TextItem, NumberItem
from canvas.items import BackgroundItem, OverlayMaskItem, SelectionItem
from canvas.undo import BatchRemoveCommand
from core import log_debug, log_info


class EraserTool(Tool):
    """
    橡皮擦工具
    
    特点：
    1. 粗暴删除法 - 每次删除整个图元
    2. 支持拖动连续擦除
    3. 批量删除 - 减少撤销栈压力
    4. 可撤销恢复
    """
    
    id = "eraser"
    
    def __init__(self):
        self.erasing = False
        self.last_pos = None
        self.erased_items = set()  # 使用 set 避免重复删除
        
    def on_press(self, pos: QPointF, button, ctx: ToolContext):
        """鼠标按下 - 开始擦除"""
        if button == Qt.MouseButton.LeftButton:
            self.erasing = True
            self.last_pos = pos
            self.erased_items.clear()
            
            # 立即擦除当前位置的图元
            self._erase_at_position(pos, ctx)
    
    def on_move(self, pos: QPointF, ctx: ToolContext):
        """鼠标移动 - 连续擦除"""
        if self.erasing and self.last_pos:
            # 创建从上一个点到当前点的路径
            path = QPainterPath()
            path.moveTo(self.last_pos)
            path.lineTo(pos)
            
            # 扩展路径为橡皮擦宽度（使用当前笔触宽度）
            stroker = QPainterPathStroker()
            stroker.setWidth(max(ctx.stroke_width, 10))  # 至少10px宽度
            stroker.setCapStyle(Qt.PenCapStyle.RoundCap)
            stroker.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
            eraser_area = stroker.createStroke(path)
            
            # 只擦除最顶层的一个图元（scene.items 返回的第一个就是最上层的）
            for item in ctx.scene.items(eraser_area):
                if self._try_erase_item(item):
                    break  # 删除一个后立即退出循环
            
            self.last_pos = pos
    
    def on_release(self, pos: QPointF, ctx: ToolContext):
        """鼠标释放 - 批量提交删除"""
        if self.erasing:
            # 最后一次擦除
            self._erase_at_position(pos, ctx)
            
            # 批量删除收集到的图元
            if self.erased_items:
                items_list = list(self.erased_items)
                cmd = BatchRemoveCommand(ctx.scene, items_list, 
                                        text=f"Erase {len(items_list)} items")
                ctx.undo_stack.push_command(cmd)
                log_info(f"删除了 {len(items_list)} 个图元", "Eraser")
            
            # 清理状态
            self.erased_items.clear()
            self.erasing = False
            self.last_pos = None
    
    def _erase_at_position(self, pos: QPointF, ctx: ToolContext):
        """在指定位置擦除图元（点击擦除）"""
        # 创建一个圆形擦除区域
        radius = max(ctx.stroke_width / 2, 5)  # 至少5px半径
        path = QPainterPath()
        path.addEllipse(pos, radius, radius)

        for item in ctx.scene.items(path):
            if self._try_erase_item(item):
                break  # 删除一个后立即退出循环th):
            self._try_erase_item(item)
    
    def _try_erase_item(self, item):
        """尝试擦除图元（跳过特殊图层）"""
        # 跳过背景、遮罩、选区框等特殊图层
        if isinstance(item, (BackgroundItem, OverlayMaskItem, SelectionItem)):
            return
        
        # 只删除可擦除的绘图图元
        if isinstance(item, (StrokeItem, RectItem, EllipseItem, ArrowItem, TextItem, NumberItem)):
            # 添加到待删除列表（使用 set 自动去重）
            self.erased_items.add(item)
            
            # 立即从场景中移除（视觉反馈）
            if item.scene():
                item.scene().removeItem(item)
    
    def on_activate(self, ctx: ToolContext):
        """工具激活 - 设置光标"""
        super().on_activate(ctx)
        # 橡皮擦光标由 CursorManager 统一管理
        log_debug("已激活", "Eraser")
    
    def on_deactivate(self, ctx: ToolContext):
        """工具停用 - 清理状态"""
        super().on_deactivate(ctx)
        
        # 如果还有未提交的删除，强制提交
        if self.erased_items:
            items_list = list(self.erased_items)
            cmd = BatchRemoveCommand(ctx.scene, items_list)
            ctx.undo_stack.push_command(cmd)
            self.erased_items.clear()
        
        self.erasing = False
        self.last_pos = None
        log_debug("已停用", "Eraser")
