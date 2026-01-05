"""
背景图层
显示截图的背景图像

内存优化：只保留 QPixmap，不存储 QImage 副本
   - 节省约 50% 内存（一张 1920x1080 截图节省 ~8MB）
   - 需要 QImage 时从 pixmap 按需转换
"""

from PyQt6.QtCore import QRectF, Qt
from PyQt6.QtGui import QImage, QPixmap
from PyQt6.QtWidgets import QGraphicsPixmapItem


class BackgroundItem(QGraphicsPixmapItem):
    """
    背景图层 - 显示截图
    Z-order: 0 (最底层)
    
    内存优化：只存储 QPixmap，需要 QImage 时按需转换
    """
    
    def __init__(self, image: QImage, scene_rect: QRectF):
        super().__init__()
        self.setZValue(0)  # 最底层
        
        self._scene_rect = QRectF(scene_rect)
        
        # 直接从 QImage 创建 QPixmap，不保留 QImage 副本
        self.setPixmap(QPixmap.fromImage(image))
        
        # 使用 setOffset 设置图像的偏移量（场景坐标）
        self.setOffset(self._scene_rect.topLeft())
        
        # 背景不可交互
        self.setAcceptedMouseButtons(Qt.MouseButton.NoButton)
        
        print(f"[OK] [背景层] 创建: scene_rect={scene_rect}, offset={self._scene_rect.topLeft()}")
    
    def image(self) -> QImage:
        """获取背景图像（按需从 pixmap 转换，避免常驻内存）"""
        return self.pixmap().toImage()
    
    def update_image(self, image: QImage):
        """更新背景图像"""
        # 直接更新 pixmap，不保留 QImage 副本
        self.setPixmap(QPixmap.fromImage(image))
