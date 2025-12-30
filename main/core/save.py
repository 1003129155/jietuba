import gc
import os
import threading
from datetime import datetime
from typing import Callable, Optional

from PyQt6.QtGui import QImage, QPixmap
from PIL import Image

from settings import get_tool_settings_manager


class SaveService:
    """Async save service for screenshots."""

    def __init__(self, config_manager=None):
        self.config_manager = config_manager or get_tool_settings_manager()

    def get_default_directory(self) -> str:
        """Return default directory based on current config."""
        return self.config_manager.get_screenshot_save_path()

    def save_pixmap_async(
        self,
        pixmap: QPixmap,
        *,
        directory: Optional[str] = None,
        prefix: str = "截图",
        suffix: str = "",
        image_format: str = "PNG",
        callback: Optional[Callable[[bool, str], None]] = None,
    ) -> Optional[str]:
        """Save a QPixmap in a background thread."""
        if pixmap is None or pixmap.isNull():
            print("[save] QPixmap is null, skip saving")
            return None

        qimage = pixmap.toImage().copy()
        return self._save_qimage_async(
            qimage,
            directory=directory,
            prefix=prefix,
            suffix=suffix,
            image_format=image_format,
            callback=callback,
        )

    def save_qimage_async(
        self,
        image: QImage,
        *,
        directory: Optional[str] = None,
        prefix: str = "截图",
        suffix: str = "",
        image_format: str = "PNG",
        callback: Optional[Callable[[bool, str], None]] = None,
    ) -> Optional[str]:
        """Save a QImage in a background thread."""
        if image is None or image.isNull():
            print("[save] QImage is null, skip saving")
            return None

        image_copy = image.copy()
        return self._save_qimage_async(
            image_copy,
            directory=directory,
            prefix=prefix,
            suffix=suffix,
            image_format=image_format,
            callback=callback,
        )

    def save_pil_async(
        self,
        pil_image: Image.Image,
        *,
        directory: Optional[str] = None,
        prefix: str = "截图",
        suffix: str = "",
        image_format: str = "PNG",
        callback: Optional[Callable[[bool, str], None]] = None,
    ) -> Optional[str]:
        """Save a PIL Image in a background thread."""
        if pil_image is None:
            print("[save] PIL image is null, skip saving")
            return None

        image_copy = pil_image.copy()
        target_path = self._compose_path(directory, prefix, suffix, image_format)

        def worker():
            nonlocal image_copy  # 允许修改外部变量
            try:
                image_copy.save(target_path, format=image_format.upper())
                print(f"[save] Saved file: {target_path}")
                if callback:
                    callback(True, target_path)
            except Exception as exc:
                print(f"[save] Failed to save {target_path}: {exc}")
                if callback:
                    callback(False, target_path)
            finally:
                # 显式清空图像引用，释放内存
                image_copy = None
                gc.collect()

        threading.Thread(target=worker, daemon=True).start()
        return target_path

    def _save_qimage_async(
        self,
        image: QImage,
        *,
        directory: Optional[str] = None,
        prefix: str,
        suffix: str,
        image_format: str,
        callback: Optional[Callable[[bool, str], None]] = None,
    ) -> Optional[str]:
        target_path = self._compose_path(directory, prefix, suffix, image_format)
        
        # 计算图像占用的内存
        image_size_mb = (image.sizeInBytes() / 1024 / 1024) if hasattr(image, 'sizeInBytes') else 0
        print(f"💾 [save] 开始保存线程，图像大小: {image.width()}x{image.height()}, 内存: {image_size_mb:.2f} MB")

        def worker():
            nonlocal image  # 允许修改外部变量
            try:
                success = image.save(target_path, image_format.upper())
                if success:
                    print(f"[save] Saved file: {target_path}")
                else:
                    print(f"[save] Failed to save {target_path}")
                if callback:
                    callback(success, target_path)
            except Exception as exc:
                print(f"[save] Failed to save {target_path}: {exc}")
                if callback:
                    callback(False, target_path)
            finally:
                # 显式删除图像引用，释放内存
                print(f"🧹 [save] 保存完成，释放图像内存: {image_size_mb:.2f} MB")
                image = None  # 清空引用而不是 del
                gc.collect()

        threading.Thread(target=worker, daemon=True).start()
        return target_path

    def _compose_path(self, directory: Optional[str], prefix: str, suffix: str, image_format: str) -> str:
        target_dir = directory or self.get_default_directory()
        os.makedirs(target_dir, exist_ok=True)
        filename = self._build_filename(prefix, suffix, image_format)
        return os.path.join(target_dir, filename)

    def _build_filename(self, prefix: str, suffix: str, image_format: str) -> str:
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        parts = [part for part in (prefix, suffix, timestamp) if part]
        base = "_".join(parts) if parts else timestamp
        return f"{base}.{image_format.lower()}"
