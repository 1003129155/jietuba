"""
选区框 - 边框和控制点
"""

from PySide6.QtCore import QRectF, QPointF, Qt
from PySide6.QtGui import QPen, QColor, QBrush, QPainter, QCursor
from PySide6.QtWidgets import (
    QGraphicsItem,
    QGraphicsTextItem,
    QStyleOptionGraphicsItem,
    QWidget,
)
from canvas.selection_model import SelectionModel
from .drawing_items import TextItem
from core.logger import log_debug
from core.theme import get_theme
from core import safe_event


class SelectionItem(QGraphicsItem):
    """
    选区框 - 显示边框和8个控制点
    Z-order: 15
    """
    
    HANDLE_SIZE = 10  # 控制点大小
    
    # 手柄标识
    HANDLE_NONE = 0
    HANDLE_TOP_LEFT = 1
    HANDLE_TOP = 2
    HANDLE_TOP_RIGHT = 3
    HANDLE_LEFT = 4
    HANDLE_RIGHT = 5
    HANDLE_BOTTOM_LEFT = 6
    HANDLE_BOTTOM = 7
    HANDLE_BOTTOM_RIGHT = 8
    HANDLE_BODY = 9 # 移动整个选区
    
    # 预缓存绘制常量，避免 paint() 每帧创建临时 Qt 对象
    _pen_handle_outer = QPen(QColor(255, 255, 255), 2)
    
    def __init__(self, model: SelectionModel):
        super().__init__()
        self.setZValue(15)
        
        self._model = model
        self._model.rectChanged.connect(self.update_bounds)
        self._model.draggingChanged.connect(self._on_dragging_changed)
        
        # 可交互（用于拖拽调整选区）
        self.setAcceptedMouseButtons(Qt.MouseButton.LeftButton)
        self.setAcceptHoverEvents(True) # 启用悬停事件以改变光标
        
        self.active_handle = self.HANDLE_NONE
        self.start_pos = QPointF()
        self.start_rect = QRectF()
        
        log_debug("选区框创建", "Canvas")
    
    def boundingRect(self) -> QRectF:
        """边界矩形"""
        if self._model.is_empty():
            return QRectF()
        
        rect = self._model.rect()
        # 扩展一点以包含边框和控制点
        return rect.adjusted(-20, -20, 20, 20)
    
    def paint(self, painter: QPainter, option: QStyleOptionGraphicsItem, widget: QWidget = None):
        """绘制选区边框、尺寸标注和控制点"""
        if self._model.is_empty():
            return
        
        rect = self._model.rect()
        
        # 绘制边框（每次从 theme 单例读取，确保颜色实时生效）
        tc = get_theme().theme_color
        painter.setPen(QPen(tc, 4, Qt.PenStyle.SolidLine))
        painter.setBrush(Qt.BrushStyle.NoBrush)
        painter.drawRect(rect)
        
        # 拖拽时或选区未确认时（智能选区预览）隐藏控制点
        # 1. 降低渲染压力
        # 2. 避免在预览时出现"画蛇添足"的手柄
        if self._model.is_dragging or not self._model.is_confirmed:
            return
        
        # 绘制8个控制点（使用预缓存的 QPen/QBrush）
        handles = self._get_handle_positions(rect)
        outer_r = self.HANDLE_SIZE // 2 + 1
        inner_r = self.HANDLE_SIZE // 2
        brush_handle = QBrush(tc)
        for pos in handles.values():
            # 外圈（白色边框 + 主题色填充）
            painter.setPen(self._pen_handle_outer)
            painter.setBrush(brush_handle)
            painter.drawEllipse(pos, outer_r, outer_r)
            
            # 内圈（纯主题色填充）
            painter.setPen(Qt.PenStyle.NoPen)
            painter.drawEllipse(pos, inner_r, inner_r)
    
    def _get_handle_positions(self, rect: QRectF) -> dict:
        """获取8个控制点的位置"""
        left = rect.left()
        right = rect.right()
        top = rect.top()
        bottom = rect.bottom()
        cx = rect.center().x()
        cy = rect.center().y()
        
        return {
            self.HANDLE_TOP_LEFT: QPointF(left, top),
            self.HANDLE_TOP: QPointF(cx, top),
            self.HANDLE_TOP_RIGHT: QPointF(right, top),
            self.HANDLE_LEFT: QPointF(left, cy),
            self.HANDLE_RIGHT: QPointF(right, cy),
            self.HANDLE_BOTTOM_LEFT: QPointF(left, bottom),
            self.HANDLE_BOTTOM: QPointF(cx, bottom),
            self.HANDLE_BOTTOM_RIGHT: QPointF(right, bottom),
        }
    
    def update_bounds(self, *_):
        """更新边界"""
        self.prepareGeometryChange()
        self.update()
    
    def _on_dragging_changed(self, is_dragging: bool):
        """拖拽状态改变时刷新显示（隐藏/显示控制点）"""
        self.update()

    def hoverMoveEvent(self, event):
        """鼠标悬停：改变光标形状"""
        # 如果不接受悬停事件，直接返回（由 Scene 控制）
        if not self.acceptHoverEvents():
            event.ignore()
            return

        if self._model.is_empty():
            super().hoverMoveEvent(event)
            return

        # 文本框有自己的编辑逻辑，不使用选区框的调整手柄
        if self._should_delegate_to_text(event.scenePos()):
            self.unsetCursor()
            event.ignore()
            super().hoverMoveEvent(event)
            return
        

        if not self._model.is_confirmed:
            # 智能选区预览状态，统一使用十字光标
            self.setCursor(QCursor(Qt.CursorShape.CrossCursor))
            super().hoverMoveEvent(event)
            return
            
        # 选区已确认，根据悬停位置显示不同的调整光标
        handle = self._hit_test(event.pos())
        cursor = Qt.CursorShape.ArrowCursor
        
        if handle == self.HANDLE_TOP_LEFT or handle == self.HANDLE_BOTTOM_RIGHT:
            cursor = Qt.CursorShape.SizeFDiagCursor
        elif handle == self.HANDLE_TOP_RIGHT or handle == self.HANDLE_BOTTOM_LEFT:
            cursor = Qt.CursorShape.SizeBDiagCursor
        elif handle == self.HANDLE_TOP or handle == self.HANDLE_BOTTOM:
            cursor = Qt.CursorShape.SizeVerCursor
        elif handle == self.HANDLE_LEFT or handle == self.HANDLE_RIGHT:
            cursor = Qt.CursorShape.SizeHorCursor
        elif handle == self.HANDLE_BODY:
            cursor = Qt.CursorShape.SizeAllCursor
            
        self.setCursor(QCursor(cursor))
        super().hoverMoveEvent(event)

    @safe_event
    def mousePressEvent(self, event):
        """鼠标按下：开始调整"""
        if event.button() == Qt.MouseButton.LeftButton:
            if self._should_delegate_to_text(event.scenePos()):
                event.ignore()
                return
            self.active_handle = self._hit_test(event.pos())
            if self.active_handle != self.HANDLE_NONE:
                self.start_pos = event.scenePos()
                self.start_rect = self._model.rect()
                # 通知开始拖拽（用于隐藏工具栏等优化）
                self._model.start_dragging()
                event.accept()
            else:
                event.ignore()
        else:
            event.ignore()

    @safe_event
    def mouseMoveEvent(self, event):
        """鼠标移动：调整选区"""
        if self.active_handle == self.HANDLE_NONE:
            return
            
        current_pos = event.scenePos()
        dx = current_pos.x() - self.start_pos.x()
        dy = current_pos.y() - self.start_pos.y()
        
        new_rect = QRectF(self.start_rect)
        
        if self.active_handle == self.HANDLE_BODY:
            new_rect.translate(dx, dy)
        else:
            if self.active_handle in [self.HANDLE_LEFT, self.HANDLE_TOP_LEFT, self.HANDLE_BOTTOM_LEFT]:
                new_rect.setLeft(self.start_rect.left() + dx)
            if self.active_handle in [self.HANDLE_RIGHT, self.HANDLE_TOP_RIGHT, self.HANDLE_BOTTOM_RIGHT]:
                new_rect.setRight(self.start_rect.right() + dx)
            if self.active_handle in [self.HANDLE_TOP, self.HANDLE_TOP_LEFT, self.HANDLE_TOP_RIGHT]:
                new_rect.setTop(self.start_rect.top() + dy)
            if self.active_handle in [self.HANDLE_BOTTOM, self.HANDLE_BOTTOM_LEFT, self.HANDLE_BOTTOM_RIGHT]:
                new_rect.setBottom(self.start_rect.bottom() + dy)
                
        self._model.set_rect(new_rect.normalized())
        event.accept()

    @safe_event
    def mouseReleaseEvent(self, event):
        """鼠标释放：结束调整"""
        if self.active_handle != self.HANDLE_NONE:
            # 通知结束拖拽（用于显示工具栏等）
            self._model.stop_dragging()
        self.active_handle = self.HANDLE_NONE
        event.accept()

    def _hit_test(self, pos: QPointF) -> int:
        """检测点击了哪个部分"""
        rect = self._model.rect()
        handles = self._get_handle_positions(rect)
        
        # 检查控制点
        for handle_id, handle_pos in handles.items():
            # 简单的距离检测
            if (pos - handle_pos).manhattanLength() < self.HANDLE_SIZE:
                return handle_id
                
        # 检查是否在矩形内部
        if rect.contains(pos):
            return self.HANDLE_BODY
            
        return self.HANDLE_NONE

    def _should_delegate_to_text(self, scene_pos: QPointF) -> bool:
        """检测当前位置是否覆盖文字图元，若是则让位于文字编辑"""
        scene = self.scene()
        if scene is None:
            return False

        for item in scene.items(scene_pos):
            if item is self:
                continue
            if not item.isVisible():
                continue
            if isinstance(item, (TextItem, QGraphicsTextItem)):
                return True
        return False
 