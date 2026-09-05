"""异步保存服务 - 处理截图文件的异步写入

提供 SaveService 类，负责将截图写入默认目录或自定义路径，
支持多线程后台保存以避免阻塞 UI。
"""

import os
import threading
from datetime import datetime
from typing import Callable, Optional

from PySide6.QtCore import QMarginsF, QRectF, QSizeF
from PySide6.QtGui import QImage, QPageLayout, QPageSize, QPainter, QPdfWriter
from PIL import Image

from settings import get_tool_settings_manager
from core.logger import log_info, log_warning, log_error, log_exception, T


class SaveService:
    """Async save service for screenshots."""

    DEFAULT_PDF_DPI = 300

    def __init__(self, config_manager=None):
        self.config_manager = config_manager or get_tool_settings_manager()

    def get_default_directory(self) -> str:
        """Return default directory based on current config."""
        return self.config_manager.get_screenshot_save_path()

    def save_qimage_async(
        self,
        image: QImage,
        *,
        directory: Optional[str] = None,
        prefix: str = "截图",
        suffix: str = "",
        image_format: str = "PNG",
        pdf_dpi: int = DEFAULT_PDF_DPI,
        callback: Optional[Callable[[bool, str], None]] = None,
    ) -> Optional[str]:
        """Save a QImage in a background thread."""
        if image is None or image.isNull():
            log_warning("QImage is null, skip saving", "Save")
            return None

        image_copy = image.copy()
        return self._save_qimage_async(
            image_copy,
            directory=directory,
            prefix=prefix,
            suffix=suffix,
            image_format=image_format,
            pdf_dpi=pdf_dpi,
            callback=callback,
        )

    def save_qimage(
        self,
        image: QImage,
        *,
        directory: Optional[str] = None,
        prefix: str = "截图",
        suffix: str = "",
        image_format: str = "PNG",
        pdf_dpi: int = DEFAULT_PDF_DPI,
    ) -> tuple[bool, Optional[str]]:
        """Save a QImage synchronously and return the result path."""
        if image is None or image.isNull():
            log_warning("QImage is null, skip saving", "Save")
            return False, None

        target_path = self._compose_path(directory, prefix, suffix, image_format)
        success = self.save_qimage_to_path(
            image,
            target_path,
            image_format=image_format,
            pdf_dpi=pdf_dpi,
        )
        return success, target_path

    def save_qimage_to_path(
        self,
        image: QImage,
        target_path: str,
        *,
        image_format: Optional[str] = None,
        pdf_dpi: int = DEFAULT_PDF_DPI,
    ) -> bool:
        """Save a QImage to an exact path, including high-quality PDF output."""
        if image is None or image.isNull():
            log_warning("QImage is null, skip saving", "Save")
            return False

        fmt = self._normalize_format(image_format or os.path.splitext(target_path)[1] or "PNG")
        if fmt == "PDF":
            return self._save_qimage_to_pdf_path(image, target_path, pdf_dpi=pdf_dpi)
        return self._save_qimage_to_path(image, target_path, fmt)

    def save_pil_async(
        self,
        pil_image: Image.Image,
        *,
        directory: Optional[str] = None,
        prefix: str = "截图",
        suffix: str = "",
        image_format: str = "PNG",
        pdf_dpi: int = DEFAULT_PDF_DPI,
        callback: Optional[Callable[[bool, str], None]] = None,
    ) -> Optional[str]:
        """Save a PIL Image in a background thread."""
        if pil_image is None:
            log_warning("PIL image is null, skip saving", "Save")
            return None

        image_copy = pil_image.copy()
        target_path = self._compose_path(directory, prefix, suffix, image_format)

        def worker():
            nonlocal image_copy  # 允许修改外部变量
            try:
                success = self._save_pil_to_path(
                    image_copy,
                    target_path,
                    image_format,
                    pdf_dpi=pdf_dpi,
                )
                if callback:
                    callback(success, target_path)
            except Exception as exc:
                log_error(T("保存失败 {target_path}: {exc}", target_path=target_path, exc=exc), "Save")
                self._cleanup_failed_placeholder(target_path)
                if callback:
                    callback(False, target_path)
            finally:
                image_copy = None  # 解除引用，PIL Image 立即释放内存

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
        pdf_dpi: int,
        callback: Optional[Callable[[bool, str], None]] = None,
    ) -> Optional[str]:
        target_path = self._compose_path(directory, prefix, suffix, image_format)

        def worker():
            nonlocal image  # 允许修改外部变量
            try:
                success = self.save_qimage_to_path(
                    image,
                    target_path,
                    image_format=image_format,
                    pdf_dpi=pdf_dpi,
                )
                if callback:
                    callback(success, target_path)
            finally:
                image = None  # 解除引用，引用计数归零时 Qt 立即释放内存

        threading.Thread(target=worker, daemon=True).start()
        return target_path

    def _save_qimage_to_path(self, image: QImage, target_path: str, image_format: str) -> bool:
        try:
            success = image.save(target_path, image_format.upper())
            if success:
                log_info(T("已保存文件: {target_path}", target_path=target_path), "Save")
            else:
                log_error(T("保存失败: {target_path}", target_path=target_path), "Save")
                self._cleanup_failed_placeholder(target_path)
            return success
        except Exception as exc:
            log_error(T("保存失败 {target_path}: {exc}", target_path=target_path, exc=exc), "Save")
            self._cleanup_failed_placeholder(target_path)
            return False

    def _save_qimage_to_pdf_path(self, image: QImage, target_path: str, *, pdf_dpi: int) -> bool:
        try:
            pdf_dpi = max(72, int(pdf_dpi))
            pdf_image = self._flatten_for_pdf(image)

            writer = QPdfWriter(target_path)
            writer.setResolution(pdf_dpi)
            writer.setPageSize(QPageSize(
                QSizeF(pdf_image.width() * 72.0 / pdf_dpi, pdf_image.height() * 72.0 / pdf_dpi),
                QPageSize.Unit.Point,
                "Screenshot",
            ))
            writer.setPageMargins(QMarginsF(0, 0, 0, 0), QPageLayout.Unit.Point)
            writer.setCreator("jietuba")

            painter = QPainter(writer)
            try:
                painter.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform, False)
                painter.drawImage(
                    QRectF(0, 0, writer.width(), writer.height()),
                    pdf_image,
                    QRectF(0, 0, pdf_image.width(), pdf_image.height()),
                )
            finally:
                painter.end()

            log_info(T("已保存PDF: {target_path}", target_path=target_path), "Save")
            return True
        except Exception as exc:
            log_error(T("保存PDF失败 {target_path}: {exc}", target_path=target_path, exc=exc), "Save")
            self._cleanup_failed_placeholder(target_path)
            return False

    def _save_pil_to_path(
        self,
        pil_image: Image.Image,
        target_path: str,
        image_format: str,
        *,
        pdf_dpi: int,
    ) -> bool:
        fmt = self._normalize_format(image_format)
        if fmt == "PDF":
            rgba = pil_image.convert("RGBA")
            data = rgba.tobytes("raw", "RGBA")
            qimage = QImage(data, rgba.width, rgba.height, rgba.width * 4, QImage.Format.Format_RGBA8888)
            return self._save_qimage_to_pdf_path(qimage.copy(), target_path, pdf_dpi=pdf_dpi)

        pil_format = "JPEG" if fmt == "JPG" else fmt
        pil_image.save(target_path, format=pil_format)
        log_info(T("已保存文件: {target_path}", target_path=target_path), "Save")
        return True

    def _flatten_for_pdf(self, image: QImage) -> QImage:
        if not image.hasAlphaChannel():
            return image.convertToFormat(QImage.Format.Format_RGB32)

        flattened = QImage(image.size(), QImage.Format.Format_RGB32)
        flattened.fill(0xFFFFFFFF)
        painter = QPainter(flattened)
        try:
            painter.drawImage(0, 0, image)
        finally:
            painter.end()
        return flattened

    def _compose_path(self, directory: Optional[str], prefix: str, suffix: str, image_format: str) -> str:
        target_dir = directory or self.get_default_directory()
        # 防御性校验：确保是绝对路径
        if not os.path.isabs(target_dir):
            log_warning(T("保存路径不是绝对路径，回退到默认: {target_dir}", target_dir=target_dir), "Save")
            target_dir = self.get_default_directory()
        os.makedirs(target_dir, exist_ok=True)
        return self._reserve_unique_path(target_dir, prefix, suffix, image_format)

    def _build_filename(self, prefix: str, suffix: str, image_format: str) -> str:
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        parts = [part for part in (prefix, suffix, timestamp) if part]
        base = "_".join(parts) if parts else timestamp
        return f"{base}.{self._normalize_format(image_format).lower()}"

    def _normalize_format(self, image_format: str) -> str:
        fmt = (image_format or "PNG").strip().lstrip(".").upper()
        return "JPG" if fmt == "JPEG" else fmt

    def _reserve_unique_path(self, target_dir: str, prefix: str, suffix: str, image_format: str) -> str:
        base_filename = self._build_filename(prefix, suffix, image_format)
        stem, ext = os.path.splitext(base_filename)

        for index in range(10000):
            filename = base_filename if index == 0 else f"{stem}_{index}{ext}"
            path = os.path.join(target_dir, filename)
            try:
                fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
                os.close(fd)
                return path
            except FileExistsError:
                continue

        raise RuntimeError(f"无法为保存文件分配唯一文件名: {base_filename}")

    def _cleanup_failed_placeholder(self, path: str) -> None:
        try:
            if os.path.exists(path) and os.path.getsize(path) == 0:
                os.remove(path)
        except Exception as e:
            log_exception(e, T("清理失败的占位文件"))
 
