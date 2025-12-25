"""
背景图层
显示截图的背景图像
"""

from PyQt6.QtCore import QRectF, Qt
from PyQt6.QtGui import QImage, QPixmap
from PyQt6.QtWidgets import QGraphicsPixmapItem


class BackgroundItem(QGraphicsPixmapItem):
    """
    背景图层 - 显示截图
    Z-order: 0 (最底层)
    """
    
    def __init__(self, image: QImage, scene_rect: QRectF):
        super().__init__()
        self.setZValue(0)  # 最底层
        
        self._scene_rect = QRectF(scene_rect)
        self._image = image.copy()
        
        # 设置 pixmap 和位置
        self.setPixmap(QPixmap.fromImage(self._image))
        # 使用 setOffset 设置图像的偏移量（场景坐标）
        self.setOffset(self._scene_rect.topLeft())
        
        # 背景不可交互
        self.setAcceptedMouseButtons(Qt.MouseButton.NoButton)
        
        print(f"✅ [背景层] 创建: scene_rect={scene_rect}, offset={self._scene_rect.topLeft()}")
    
    def image(self) -> QImage:
        """获取背景图像"""
        return self._image
    
    def update_image(self, image: QImage):
        """更新背景图像"""
        self._image = image.copy()
        self.setPixmap(QPixmap.fromImage(self._image))
