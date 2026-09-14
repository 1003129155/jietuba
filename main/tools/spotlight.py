"""
聚光灯工具
"""

from PySide6.QtCore import QPointF, QRectF, Qt

from .base import Tool, ToolContext
from canvas.items import SpotlightCurtain, SpotlightItem
from canvas.undo import AddItemCommand


class SpotlightTool(Tool):
    """拖出一个矩形孔，孔外的整片区域被幕布压暗。"""

    id = "spotlight"

    # 最小绘制尺寸（像素），小于此值的绘制将被忽略
    MIN_SIZE = 10

    def __init__(self):
        self.drawing = False
        self.start_pos = None
        self.current_item = None

    def on_press(self, pos: QPointF, button, ctx: ToolContext):
        if button != Qt.MouseButton.LeftButton:
            return
        self.drawing = True
        self.start_pos = pos
        # 幕布整个场景只有一张：画新孔时按工具当前的透明度统一暗度，已有的孔一起变
        SpotlightCurtain.of(ctx.scene).setOpacity(max(0.0, min(1.0, float(ctx.opacity))))
        self.current_item = SpotlightItem(QRectF(pos, pos))
        ctx.scene.addItem(self.current_item)

    def on_move(self, pos: QPointF, ctx: ToolContext):
        if self.drawing and self.current_item:
            self.current_item.setRect(QRectF(self.start_pos, pos).normalized())

    def on_release(self, pos: QPointF, ctx: ToolContext):
        if not self.drawing:
            return
        self.drawing = False
        item, self.current_item = self.current_item, None
        if item is None:
            return

        # 先移出场景：留不留下由撤销栈决定，尺寸太小就直接丢掉
        ctx.scene.removeItem(item)
        rect = QRectF(self.start_pos, pos).normalized()
        if rect.width() < self.MIN_SIZE or rect.height() < self.MIN_SIZE:
            return

        ctx.undo_stack.push(AddItemCommand(ctx.scene, item))
        # 绘制完成后自动选中（方便调整）
        ctx.scene.item_auto_select_requested.emit(item)
