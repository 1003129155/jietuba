"""
选区框 - 边框和控制点
"""

from PyQt6.QtCore import QRectF, QPointF, Qt
from PyQt6.QtGui import QPen, QColor, QBrush, QPainter, QCursor
from PyQt6.QtWidgets import (
    QGraphicsItem,
    QGraphicsTextItem,
    QStyleOptionGraphicsItem,
    QWidget,
)
from canvas.model import SelectionModel
from .drawing_items import TextItem


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
    
    def __init__(self, model: SelectionModel):
        super().__init__()
        self.setZValue(15)
        
        self._model = model
        self._model.rectChanged.connect(self.update_bounds)
        
        # 可交互（用于拖拽调整选区）
        self.setAcceptedMouseButtons(Qt.MouseButton.LeftButton)
        self.setAcceptHoverEvents(True) # 启用悬停事件以改变光标
        
        self.active_handle = self.HANDLE_NONE
        self.start_pos = QPointF()
        self.start_rect = QRectF()
        
        print(f"[OK] [选区框] 创建")
    
    def boundingRect(self) -> QRectF:
        """边界矩形"""
        if self._model.is_empty():
            return QRectF()
        
        rect = self._model.rect()
        # 扩展一点以包含边框和控制点
        return rect.adjusted(-20, -20, 20, 20)
    
    def paint(self, painter: QPainter, option: QStyleOptionGraphicsItem, widget: QWidget = None):
        """绘制选区边框和控制点"""
        if self._model.is_empty():
            return
        
        rect = self._model.rect()
        
        # 绘制边框（加粗到4像素，与老代码一致）
        pen = QPen(QColor(64, 224, 208), 4, Qt.PenStyle.SolidLine)  # 青色，4px
        painter.setPen(pen)
        painter.setBrush(Qt.BrushStyle.NoBrush)
        painter.drawRect(rect)
        
        # 绘制8个控制点
        handles = self._get_handle_positions(rect)
        for pos in handles.values():
            # 外圈（白色，2px）
            painter.setPen(QPen(QColor(255, 255, 255), 2))
            painter.setBrush(QColor(48, 200, 192))
            painter.drawEllipse(pos, self.HANDLE_SIZE // 2 + 1, self.HANDLE_SIZE // 2 + 1)
            
            # 内圈（青绿色填充）
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(QColor(48, 200, 192))
            painter.drawEllipse(pos, self.HANDLE_SIZE // 2, self.HANDLE_SIZE // 2)
    
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
        
        # 区分智能选区预览和真实选区
        # 智能选区预览（未确认）：显示十字瞄准光标
        # 真实选区（已确认）：显示调整光标
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
                event.accept()
            else:
                event.ignore()
        else:
            event.ignore()

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

    def mouseReleaseEvent(self, event):
        """鼠标释放：结束调整"""
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
