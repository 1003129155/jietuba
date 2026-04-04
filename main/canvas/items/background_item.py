"""
背景图层
显示截图的背景图像


"""

from PySide6.QtCore import QRectF, Qt
from PySide6.QtGui import QImage, QPixmap
from PySide6.QtWidgets import QGraphicsPixmapItem
from core.logger import log_debug


class BackgroundItem(QGraphicsPixmapItem):
    """
    背景图层 - 显示截图
    Z-order: 0 (最底层)
    
    同时缓存 QPixmap 和 QImage，避免放大镜每帧调用 pixmap().toImage() 造成卡顿。
    截图窗口 cleanup_and_close() 销毁 scene 时，本对象及缓存会一起被清理。
    """
    
    def __init__(self, image: QImage, scene_rect: QRectF):
        super().__init__()
        self.setZValue(0)  # 最底层
        
        self._scene_rect = QRectF(scene_rect)
        
        # 直接引用外部 QImage（供放大镜高频读取），不再 copy()
        # 原因：image 来自 ScreenshotWindow.original_image，生命周期覆盖本对象，
        # 且全程只读，无需防御性拷贝。省掉一次全屏内存拷贝（1080p≈8MB, 4K≈32MB）
        self._cached_image = image
        self.setPixmap(QPixmap.fromImage(image))
        
        # 使用 setOffset 设置图像的偏移量（场景坐标）
        self.setOffset(self._scene_rect.topLeft())
        
        # 背景不可交互
        self.setAcceptedMouseButtons(Qt.MouseButton.NoButton)
        
        log_debug(f"背景层创建: scene_rect={scene_rect}, offset={self._scene_rect.topLeft()}", "Canvas")
    
    def image(self) -> QImage:
        """获取背景图像（直接返回缓存，零开销）"""
        if self._cached_image is not None:
            return self._cached_image
        # 极端情况兜底：缓存被意外清除时重建
        self._cached_image = self.pixmap().toImage()
        return self._cached_image
    
    def update_image(self, image: QImage):
        """更新背景图像"""
        self._cached_image = image  # 直接引用，不拷贝
        self.setPixmap(QPixmap.fromImage(image))
    
    def release_image_cache(self):
        """主动释放 QImage 缓存（节省内存，钉图等场景可调用）"""
        self._cached_image = None
 