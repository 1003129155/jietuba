# -*- coding: utf-8 -*-

"""图片条目的表单。

编辑已有图片：标题、原图预览、尺寸信息和复制 / 钉图 / 另存为；图片本身不可编辑，保存时只改标题。
新增图片：拖入、粘贴或选择一张图，经由剪贴板记录进库后移入分组。
"""

from typing import Optional

from PySide6.QtCore import QRect, QSize, Qt, Signal
from PySide6.QtGui import QColor, QFont, QImage, QPainter, QPen, QPixmap
from PySide6.QtWidgets import QHBoxLayout, QSizePolicy, QVBoxLayout, QWidget

from core import safe_event
from core.ui_scale import dialog_scaled
from core.ui_theme import get_ui_theme
from ui.fluent_lite import LineEdit, PushButton as FluentPushButton
from ui.fluent_lite.theme import css_color
from .form_widgets import field_hint, field_label

_PREVIEW_MAX_HEIGHT = 340
# 新增页下方还有按钮和说明，预览矮一些，免得一放图就出滚动条
_NEW_IMAGE_PREVIEW_MAX_HEIGHT = 240
_CHECKER = 8


def _format_bytes(size: int) -> str:
    if size >= 1024 * 1024:
        return f"{size / 1024 / 1024:.1f} MB"
    return f"{max(1, round(size / 1024))} KB"


class ImagePreview(QWidget):
    """按比例缩小显示原图，不放大；透明区域衬棋盘格。"""

    double_clicked = Signal()

    def __init__(self, image: QImage, parent=None, max_height: int = _PREVIEW_MAX_HEIGHT):
        super().__init__(parent)
        self._image = image
        self._max_height = max_height
        self._cache_key = None
        self._scaled: Optional[QPixmap] = None
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        get_ui_theme().theme_changed.connect(self.update)

    def _box_height(self, width: int) -> int:
        pad = dialog_scaled(12) * 2
        inner_w = max(1, width - pad)
        ratio = self._image.height() / max(1, self._image.width())
        fitted = min(self._image.height() / self.devicePixelRatioF(), inner_w * ratio)
        return int(min(fitted, dialog_scaled(self._max_height))) + pad

    def sizeHint(self) -> QSize:
        width = self.width() if self.width() > 0 else dialog_scaled(420)
        return QSize(width, self._box_height(width))

    def resizeEvent(self, event):
        super().resizeEvent(event)
        height = self._box_height(self.width())
        if self.height() != height:
            self.setFixedHeight(height)

    def image_rect(self) -> QRect:
        pad = dialog_scaled(12)
        area = self.rect().adjusted(pad, pad, -pad, -pad)
        dpr = self.devicePixelRatioF()
        # 原图按物理像素显示时不超过 1:1，小图不会被放大发糊
        natural = QSize(round(self._image.width() / dpr), round(self._image.height() / dpr))
        size = natural.scaled(area.size(), Qt.AspectRatioMode.KeepAspectRatio) \
            if natural.width() > area.width() or natural.height() > area.height() else natural
        rect = QRect(0, 0, size.width(), size.height())
        rect.moveCenter(area.center())
        return rect

    def _scaled_pixmap(self, rect: QRect) -> QPixmap:
        dpr = self.devicePixelRatioF()
        key = (rect.width(), rect.height(), dpr)
        if key != self._cache_key:
            pixmap = QPixmap.fromImage(self._image.scaled(
                round(rect.width() * dpr), round(rect.height() * dpr),
                Qt.AspectRatioMode.KeepAspectRatio, Qt.TransformationMode.SmoothTransformation,
            ))
            pixmap.setDevicePixelRatio(dpr)
            self._scaled = pixmap
            self._cache_key = key
        return self._scaled

    @safe_event
    def paintEvent(self, _event):
        tokens = get_ui_theme().tokens
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        radius = dialog_scaled(10)
        painter.setPen(css_color(tokens.border))
        painter.setBrush(css_color(tokens.surface_subtle))
        painter.drawRoundedRect(self.rect().adjusted(0, 0, -1, -1), radius, radius)

        rect = self.image_rect()
        if self._image.hasAlphaChannel():
            light = QColor(255, 255, 255) if not tokens.is_dark else QColor(70, 74, 80)
            dark = QColor(228, 231, 235) if not tokens.is_dark else QColor(56, 60, 66)
            cell = dialog_scaled(_CHECKER)
            painter.save()
            painter.setClipRect(rect)
            for y in range(rect.top(), rect.bottom() + 1, cell):
                for x in range(rect.left(), rect.right() + 1, cell):
                    odd = ((x - rect.left()) // cell + (y - rect.top()) // cell) % 2
                    painter.fillRect(x, y, cell, cell, dark if odd else light)
            painter.restore()
        painter.drawPixmap(rect, self._scaled_pixmap(rect))
        painter.end()

    def mouseDoubleClickEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton:
            self.double_clicked.emit()
            return
        super().mouseDoubleClickEvent(event)


def build_image_content_form(dialog, item, image: Optional[QImage], data_size: int):
    """图片条目编辑表单。image 为 None 表示原图已不可用。"""
    dialog.detail_layout.addWidget(field_label(dialog.tr("Title (Optional)"), top_gap=False))
    dialog.title_input = LineEdit()
    dialog.title_input.setPlaceholderText(dialog.tr("Enter title..."))
    dialog.title_input.setText(item.title or "")
    dialog.detail_layout.addWidget(dialog.title_input)

    dialog.detail_layout.addWidget(field_label(dialog.tr("Image")))
    if image is None:
        dialog.detail_layout.addWidget(field_hint(dialog.tr("The original image is no longer available")))
        dialog.image_preview = None
    else:
        dialog.image_preview = ImagePreview(image)
        dialog.image_preview.setToolTip(dialog.tr("Double-click to pin it on screen"))
        dialog.image_preview.double_clicked.connect(dialog._pin_image_item)
        dialog.detail_layout.addWidget(dialog.image_preview)

        info = [f"{image.width()} × {image.height()}", "PNG", _format_bytes(data_size)]
        if item.source_app:
            info.append(dialog.tr("From {app}").format(app=item.source_app))
        dialog.detail_layout.addWidget(field_hint(" · ".join(info)))

    actions = QHBoxLayout()
    actions.setSpacing(dialog_scaled(8))
    for text, handler in (
        (dialog.tr("Copy"), dialog._copy_image_item),
        (dialog.tr("Pin to Screen"), dialog._pin_image_item),
        (dialog.tr("Save As..."), dialog._save_image_item_as),
    ):
        button = FluentPushButton(text)
        button.setCursor(Qt.CursorShape.PointingHandCursor)
        button.setEnabled(image is not None)
        button.clicked.connect(handler)
        actions.addWidget(button)
    actions.addStretch()
    dialog.detail_layout.addLayout(actions)
    dialog.detail_layout.addStretch()


_IMAGE_SUFFIXES = (".png", ".jpg", ".jpeg", ".bmp", ".gif", ".webp", ".tif", ".tiff", ".ico")


def image_from_mime(mime) -> Optional[QImage]:
    """拖进来的图片：优先取图片数据本身，其次取第一个图片文件。"""
    if mime.hasImage():
        image = QImage(mime.imageData())
        if not image.isNull():
            return image
    for url in mime.urls() if mime.hasUrls() else ():
        path = url.toLocalFile()
        if path.lower().endswith(_IMAGE_SUFFIXES):
            image = QImage(path)
            if not image.isNull():
                return image
    return None


class ImageDropZone(QWidget):
    """新增图片时的放置区：没图时是虚线提示框，有图后换成预览。"""

    image_dropped = Signal(QImage)

    def __init__(self, prompt: str, parent=None):
        super().__init__(parent)
        self._prompt = prompt
        self._preview: Optional[ImagePreview] = None
        self._drag_active = False
        self.setAcceptDrops(True)
        self.setMinimumHeight(dialog_scaled(150))
        self._layout = QVBoxLayout(self)
        self._layout.setContentsMargins(0, 0, 0, 0)
        get_ui_theme().theme_changed.connect(self.update)

    def set_image(self, image: Optional[QImage]):
        if self._preview is not None:
            self._layout.removeWidget(self._preview)
            self._preview.hide()
            self._preview.deleteLater()
            self._preview = None
        if image is not None:
            self._preview = ImagePreview(image, self, max_height=_NEW_IMAGE_PREVIEW_MAX_HEIGHT)
            self._preview.setCursor(Qt.CursorShape.ArrowCursor)
            self._layout.addWidget(self._preview)
        self.update()

    @safe_event
    def paintEvent(self, _event):
        if self._preview is not None:
            return
        tokens = get_ui_theme().tokens
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        pen = QPen(css_color(tokens.accent if self._drag_active else tokens.border_hover), dialog_scaled(2))
        pen.setStyle(Qt.PenStyle.DashLine)
        painter.setPen(pen)
        painter.setBrush(css_color(tokens.accent_soft if self._drag_active else tokens.surface_subtle))
        radius = dialog_scaled(10)
        painter.drawRoundedRect(self.rect().adjusted(1, 1, -1, -1), radius, radius)
        font = QFont(self.font())
        font.setPixelSize(dialog_scaled(13))
        painter.setFont(font)
        painter.setPen(css_color(tokens.accent if self._drag_active else tokens.text_muted))
        painter.drawText(self.rect(), Qt.AlignmentFlag.AlignCenter | Qt.TextFlag.TextWordWrap, self._prompt)
        painter.end()

    def _set_drag_active(self, active: bool):
        self._drag_active = active
        self.update()

    def dragEnterEvent(self, event):
        if image_from_mime(event.mimeData()) is not None:
            event.acceptProposedAction()
            self._set_drag_active(True)
            return
        event.ignore()

    def dragLeaveEvent(self, event):
        self._set_drag_active(False)
        super().dragLeaveEvent(event)

    def dropEvent(self, event):
        self._set_drag_active(False)
        image = image_from_mime(event.mimeData())
        if image is None:
            event.ignore()
            return
        event.acceptProposedAction()
        self.image_dropped.emit(image)


def build_new_image_form(dialog, monitoring: bool):
    """新增图片：图片要经由剪贴板记录进库，所以记录关着时整页不可用。"""
    dialog.detail_layout.addWidget(field_label(dialog.tr("Title (Optional)"), top_gap=False))
    dialog.title_input = LineEdit()
    dialog.title_input.setPlaceholderText(dialog.tr("Enter title..."))
    dialog.detail_layout.addWidget(dialog.title_input)

    dialog.detail_layout.addWidget(field_label(dialog.tr("Image")))
    dialog.image_drop_zone = ImageDropZone(dialog.tr("Drag an image here"))
    dialog.image_drop_zone.image_dropped.connect(dialog._set_pending_image)
    dialog.image_drop_zone.setEnabled(monitoring)
    dialog.detail_layout.addWidget(dialog.image_drop_zone)

    actions = QHBoxLayout()
    actions.setSpacing(dialog_scaled(8))
    dialog.paste_image_btn = FluentPushButton(dialog.tr("Paste from Clipboard"))
    dialog.paste_image_btn.clicked.connect(dialog._use_clipboard_image)
    browse_btn = FluentPushButton(dialog.tr("Choose Image File"))
    browse_btn.clicked.connect(dialog._browse_image_file)
    for button in (dialog.paste_image_btn, browse_btn):
        button.setCursor(Qt.CursorShape.PointingHandCursor)
        button.setEnabled(monitoring)
        actions.addWidget(button)
    actions.addStretch()
    dialog.detail_layout.addLayout(actions)

    if monitoring:
        hint = dialog.tr("The image is saved through clipboard history, so it will replace what is currently on the clipboard")
    else:
        hint = dialog.tr("Turn on clipboard history first: images are saved through it")
    dialog.detail_layout.addWidget(field_hint(hint))
    dialog.detail_layout.addStretch()
