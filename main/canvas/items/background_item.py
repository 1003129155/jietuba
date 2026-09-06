"""
背景图层
显示截图的背景图像


"""

from PySide6.QtCore import QRectF, Qt
from PySide6.QtGui import QImage, QPixmap
from PySide6.QtWidgets import QGraphicsPixmapItem
from PIL import Image
from core.logger import log_debug, T


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
        # 只保留最近一次的缩小图：block_size 一旦变了，旧的就没有复用价值
        # （马赛克粒度滑动条改了就会换新粒度重画），没必要按 block_size 攒多份。
        self._reduced_cache_key = None
        self._reduced_cache_image = None
        self.setPixmap(QPixmap.fromImage(image))
        
        # 使用 setOffset 设置图像的偏移量（场景坐标）
        self.setOffset(self._scene_rect.topLeft())
        
        # 背景不可交互
        self.setAcceptedMouseButtons(Qt.MouseButton.NoButton)
        
        log_debug(T("背景层创建: scene_rect={scene_rect}, offset={offset}", scene_rect=scene_rect, offset=self._scene_rect.topLeft()), "Canvas")
    
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
        self._reduced_cache_key = None
        self._reduced_cache_image = None
        self.setPixmap(QPixmap.fromImage(image))

    def reduced_image(self, block_size: int) -> QImage:
        """Return the background shrunk block_size times, for the mosaic to blow back up.

        马赛克在 paint 时按块放大这张小图，所以这里不再造一张全分辨率的像素化图：
        那张图每个 block 内部都是同一个颜色，98% 的字节是重复的（4K 下 33MB 对
        0.52MB）。缩小仍然走 PIL.reduce，因为它对图像右/下边缘不足一个 block 的
        余数只按实际像素取均值，Qt 的 scaled 会把邻近块混进来。

        缓存只留最近一次算出来的那张：已经画到图元上的笔画各自持有自己那份
        QImage 的引用，不依赖这里的缓存继续存在，所以换 block_size 时旧的
        直接丢，不必按 block_size 攒一个字典。
        """
        block_size = max(2, int(block_size))
        if self._reduced_cache_key == block_size and self._reduced_cache_image is not None:
            return QImage(self._reduced_cache_image)

        source = self.image()
        if source.isNull():
            return QImage()
        rgba = source.convertToFormat(QImage.Format.Format_RGBA8888)
        pil_source = Image.frombytes(
            "RGBA",
            (rgba.width(), rgba.height()),
            bytes(rgba.bits()),
            "raw",
            "RGBA",
            rgba.bytesPerLine(),
        )
        reduced = pil_source.reduce(block_size)
        small = QImage(
            reduced.tobytes("raw", "RGBA"),
            reduced.width,
            reduced.height,
            QImage.Format.Format_RGBA8888,
        ).copy()
        self._reduced_cache_key = block_size
        self._reduced_cache_image = small
        return QImage(small)

    def release_image_cache(self):
        """主动释放 QImage 缓存（节省内存，钉图等场景可调用）"""
        self._cached_image = None
        self._reduced_cache_key = None
        self._reduced_cache_image = None
