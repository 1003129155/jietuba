"""
绘制层 - Overlay 位图层
承载所有绘图工具的绘制结果
"""

from PyQt6.QtCore import QRectF, QPointF, Qt
from PyQt6.QtGui import QImage, QPainter, QPixmap
from PyQt6.QtWidgets import QGraphicsPixmapItem


class OverlayPixmapItem(QGraphicsPixmapItem):
    """
    Overlay 位图层 - 承载所有绘图
    Z-order: 20
    """
    
    def __init__(self, scene_rect: QRectF):
        super().__init__()
        self.setZValue(20)
        
        self._scene_rect = QRectF(scene_rect)
        
        # 创建透明 overlay（逻辑坐标尺寸）
        w = max(1, int(self._scene_rect.width()))
        h = max(1, int(self._scene_rect.height()))
        self._img = QImage(w, h, QImage.Format.Format_ARGB32_Premultiplied)
        self._img.fill(0)  # 透明
        
        self.setPixmap(QPixmap.fromImage(self._img))
        self.setOffset(self._scene_rect.topLeft())
        
        print(f"✅ [绘制层] 创建: {scene_rect}, 大小: {w}x{h}")
    
    def image(self) -> QImage:
        """获取内部图像（可直接用 QPainter 绘制）"""
        return self._img
    
    def clear(self):
        """清空绘制层"""
        self._img.fill(0)
        self.setPixmap(QPixmap.fromImage(self._img))
    
    def mark_dirty(self, dirty_scene_rect: QRectF = None):
        """
        标记脏区域并更新显示
        
        Args:
            dirty_scene_rect: 脏区域（场景坐标），None = 全部更新
        """
        # 更新 pixmap
        self.setPixmap(QPixmap.fromImage(self._img))
        
        # 更新显示
        if dirty_scene_rect:
            self.update(dirty_scene_rect)
        else:
            self.update()
    
    def scene_to_image_pos(self, scene_pos: QPointF) -> QPointF:
        """
        场景坐标 → 图像坐标
        
        Args:
            scene_pos: 场景坐标点
            
        Returns:
            图像坐标点
        """
        offset = self.offset()
        return QPointF(scene_pos.x() - offset.x(), scene_pos.y() - offset.y())
    
    def image_to_scene_pos(self, image_pos: QPointF) -> QPointF:
        """
        图像坐标 → 场景坐标
        
        Args:
            image_pos: 图像坐标点
            
        Returns:
            场景坐标点
        """
        offset = self.offset()
        return QPointF(image_pos.x() + offset.x(), image_pos.y() + offset.y())
