"""
高亮层 - 专门用于荧光笔绘制，支持正片叠底
"""

from PyQt6.QtCore import QRectF, QPointF, Qt
from PyQt6.QtGui import QImage, QPainter, QPixmap
from PyQt6.QtWidgets import QGraphicsPixmapItem


class HighlighterItem(QGraphicsPixmapItem):
    """
    高亮层
    Z-order: 15 (在背景之上，普通绘图层之下)
    """
    
    def __init__(self, scene_rect: QRectF):
        super().__init__()
        self.setZValue(15)
        
        self._scene_rect = QRectF(scene_rect)
        
        # 创建透明画布
        w = max(1, int(self._scene_rect.width()))
        h = max(1, int(self._scene_rect.height()))
        self._img = QImage(w, h, QImage.Format.Format_ARGB32_Premultiplied)
        self._img.fill(0)  # 透明
        
        self.setPixmap(QPixmap.fromImage(self._img))
        self.setOffset(self._scene_rect.topLeft())
        
    def paint(self, painter, option, widget):
        """
        重写绘制方法，应用正片叠底混合模式
        """
        # 设置混合模式为正片叠底 (Multiply)
        # 注意：这会影响该 Item 绘制到 Scene 上的方式
        painter.setCompositionMode(QPainter.CompositionMode.CompositionMode_Multiply)
        super().paint(painter, option, widget)
        
    def image(self) -> QImage:
        """获取内部图像"""
        return self._img
    
    def clear(self):
        """清空"""
        self._img.fill(0)
        self.setPixmap(QPixmap.fromImage(self._img))
    
    def mark_dirty(self, dirty_scene_rect: QRectF = None):
        """更新显示"""
        self.setPixmap(QPixmap.fromImage(self._img))
        if dirty_scene_rect:
            self.update(dirty_scene_rect)
        else:
            self.update()
            
    def scene_to_image_pos(self, scene_pos: QPointF) -> QPointF:
        offset = self.offset()
        return QPointF(scene_pos.x() - offset.x(), scene_pos.y() - offset.y())
