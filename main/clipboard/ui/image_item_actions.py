# -*- coding: utf-8 -*-

"""图片条目的通用操作：读取原图、另存为。剪贴板窗口和管理窗口共用。"""

import os
import re
from typing import Optional

from PySide6.QtCore import QCoreApplication
from PySide6.QtGui import QImage
from PySide6.QtWidgets import QFileDialog, QWidget

from ui.dialogs import show_warning_dialog


def _tr(text: str) -> str:
    # 这些文案最早写在剪贴板窗口里，沿用它的翻译上下文，已有译文不用重录。
    return QCoreApplication.translate("ClipboardWindow", text)


def load_item_image(manager, item) -> Optional[QImage]:
    """读取图片条目的原图（不是列表里的缩略图）。"""
    if item is None or item.content_type != "image" or not item.image_id:
        return None
    data = manager.get_image_data(item.image_id)
    if not data:
        return None
    image = QImage()
    return image if image.loadFromData(data) else None


def save_image_item_as(parent: QWidget, manager, item) -> None:
    if item is None or item.content_type != "image" or not item.image_id:
        return
    image = load_item_image(manager, item)
    if image is None:
        show_warning_dialog(parent, _tr("Save Failed"), _tr("Image data is unavailable."))
        return

    if item.created_at:
        default_name = f"clipboard_image_{item.created_at.strftime('%Y%m%d_%H%M%S')}.png"
    else:
        default_name = f"clipboard_image_{item.id}.png"

    file_path, selected_filter = QFileDialog.getSaveFileName(
        parent,
        _tr("Save as"),
        default_name,
        _tr("PNG Image (*.png);;JPEG Image (*.jpg *.jpeg);;Bitmap Image (*.bmp);;WebP Image (*.webp);;PDF (*.pdf)"),
    )
    if not file_path:
        return

    image_format = format_from_save_filter(file_path, selected_filter)
    if not os.path.splitext(file_path)[1]:
        file_path = f"{file_path}.{image_format.lower()}"

    from core.save import SaveService

    if not SaveService().save_qimage_to_path(image, file_path, image_format=image_format):
        show_warning_dialog(parent, _tr("Save Failed"), _tr("Failed to save image."))


def format_from_save_filter(file_path: str, selected_filter: str) -> str:
    ext = os.path.splitext(file_path)[1].lstrip(".")
    if ext:
        return ext.upper()
    # 从过滤器字符串中提取第一个扩展名，如 "JPEG (*.jpg *.jpeg)" → "jpg"
    match = re.search(r'\*\.(\w+)', selected_filter)
    if match:
        return match.group(1).upper()
    return "PNG"
