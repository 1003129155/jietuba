# -*- coding: utf-8 -*-

"""管理窗口左侧分组列表和中间内容列表的行绘制。"""

import base64
from functools import lru_cache
from typing import Optional

from PySide6.QtCore import QPointF, QRect, QRectF, QSize, Qt
from PySide6.QtGui import QFont, QFontMetrics, QPainter, QPen, QPixmap
from PySide6.QtWidgets import QStyle, QStyledItemDelegate

from core.ui_scale import dialog_scaled, dialog_scaled_f
from core.ui_theme import get_ui_theme
from ui.fluent_lite.theme import css_color

ICON_ROLE = Qt.ItemDataRole.UserRole + 1
TAG_ROLE = Qt.ItemDataRole.UserRole + 2
THUMB_ROLE = Qt.ItemDataRole.UserRole + 3
META_ROLE = Qt.ItemDataRole.UserRole + 4
# 搜索用的完整文本（文本条目是全文，文件条目是完整路径），不显示
SEARCH_ROLE = Qt.ItemDataRole.UserRole + 5


def _font(base: QFont, pixel_size: int, weight=QFont.Weight.Normal) -> QFont:
    font = QFont(base)
    font.setPixelSize(dialog_scaled(pixel_size))
    font.setWeight(weight)
    return font


def _paint_row_background(painter: QPainter, option, rect: QRect):
    tokens = get_ui_theme().tokens
    selected = bool(option.state & QStyle.StateFlag.State_Selected)
    hovered = bool(option.state & QStyle.StateFlag.State_MouseOver)
    if not (selected or hovered):
        return
    radius = dialog_scaled(8)
    painter.setPen(Qt.PenStyle.NoPen)
    if selected:
        painter.setBrush(css_color(tokens.accent_soft))
    else:
        hover = css_color(tokens.accent_soft)
        hover.setAlphaF(0.45)
        painter.setBrush(hover)
    painter.drawRoundedRect(QRectF(rect), radius, radius)
    if selected:
        bar_height = min(rect.height() - dialog_scaled(16), dialog_scaled(18))
        bar = QRectF(rect.left(), rect.center().y() - bar_height / 2, dialog_scaled(3), bar_height)
        painter.setBrush(css_color(tokens.accent))
        painter.drawRoundedRect(bar, dialog_scaled_f(1.5), dialog_scaled_f(1.5))


class GroupRowDelegate(QStyledItemDelegate):
    """分组行：图标 + 名称，快速启动 / 隐藏分组在右侧带一个小标签。"""

    def sizeHint(self, option, index):
        return QSize(option.rect.width(), dialog_scaled(38))

    def paint(self, painter: QPainter, option, index):
        tokens = get_ui_theme().tokens
        painter.save()
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        view = self.parent()
        if hasattr(view, "is_row_droppable") and not view.is_row_droppable(index.row()):
            painter.setOpacity(0.35)
        rect = option.rect.adjusted(dialog_scaled(4), dialog_scaled(2), -dialog_scaled(4), -dialog_scaled(2))
        _paint_row_background(painter, option, rect)

        x = rect.left() + dialog_scaled(12)
        icon_font = _font(option.font, 15)
        painter.setFont(icon_font)
        painter.setPen(css_color(tokens.text))
        icon_rect = QRect(x, rect.top(), dialog_scaled(22), rect.height())
        painter.drawText(icon_rect, Qt.AlignmentFlag.AlignCenter, index.data(ICON_ROLE) or "")
        x = icon_rect.right() + dialog_scaled(10)

        right = rect.right() - dialog_scaled(10)
        tag = index.data(TAG_ROLE) or ""
        if tag:
            tag_font = _font(option.font, 10)
            metrics = QFontMetrics(tag_font)
            pad = dialog_scaled(6)
            tag_w = metrics.horizontalAdvance(tag) + pad * 2
            tag_h = metrics.height() + dialog_scaled(2)
            tag_rect = QRectF(right - tag_w, rect.center().y() - tag_h / 2, tag_w, tag_h)
            painter.setPen(Qt.PenStyle.NoPen)
            fill = css_color(tokens.accent_soft)
            painter.setBrush(fill)
            painter.drawRoundedRect(tag_rect, tag_h / 2, tag_h / 2)
            painter.setFont(tag_font)
            painter.setPen(css_color(tokens.accent_text))
            painter.drawText(tag_rect, Qt.AlignmentFlag.AlignCenter, tag)
            right = int(tag_rect.left()) - dialog_scaled(6)

        selected = bool(option.state & QStyle.StateFlag.State_Selected)
        name_font = _font(option.font, 13, QFont.Weight.DemiBold if selected else QFont.Weight.Normal)
        painter.setFont(name_font)
        painter.setPen(css_color(tokens.text))
        name_rect = QRect(x, rect.top(), max(0, right - x), rect.height())
        name = QFontMetrics(name_font).elidedText(
            index.data(Qt.ItemDataRole.DisplayRole) or "", Qt.TextElideMode.ElideRight, name_rect.width()
        )
        painter.drawText(name_rect, Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignLeft, name)
        painter.restore()


class ContentRowDelegate(QStyledItemDelegate):
    """内容行：单行显示，左侧图标或缩略图，右侧日期；与剪贴板面板的列表一致。"""

    def sizeHint(self, option, index):
        return QSize(option.rect.width(), dialog_scaled(34))

    def paint(self, painter: QPainter, option, index):
        tokens = get_ui_theme().tokens
        painter.save()
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        rect = option.rect.adjusted(dialog_scaled(6), dialog_scaled(1), -dialog_scaled(6), -dialog_scaled(1))
        _paint_row_background(painter, option, rect)

        inner = rect.adjusted(dialog_scaled(14), 0, -dialog_scaled(12), 0)
        if index.row() < index.model().rowCount() - 1:
            pen = QPen(css_color(tokens.border), 1)
            pen.setStyle(Qt.PenStyle.DashLine)
            painter.setPen(pen)
            y = option.rect.bottom() + 0.5
            painter.drawLine(QPointF(inner.left(), y), QPointF(inner.right(), y))
        x = inner.left()

        thumbnail = _thumbnail(index.data(THUMB_ROLE) or "")
        icon = index.data(ICON_ROLE) or ""
        if thumbnail is not None:
            side = inner.height() - dialog_scaled(8)
            scaled = thumbnail.scaled(
                side * 2, side * 2, Qt.AspectRatioMode.KeepAspectRatio,
                Qt.TransformationMode.SmoothTransformation,
            )
            target = QRect(x, inner.top() + (inner.height() - side) // 2, side, side)
            fitted = scaled.size().scaled(target.size(), Qt.AspectRatioMode.KeepAspectRatio)
            painter.drawPixmap(
                QRect(target.left(), target.top() + (side - fitted.height()) // 2, fitted.width(), fitted.height()),
                scaled,
            )
            x += side + dialog_scaled(8)
        elif icon:
            painter.setFont(_font(option.font, 13))
            painter.setPen(css_color(tokens.text))
            icon_w = dialog_scaled(20)
            painter.drawText(QRect(x, inner.top(), icon_w, inner.height()), Qt.AlignmentFlag.AlignCenter, icon)
            x += icon_w + dialog_scaled(6)

        right = inner.right()
        meta = index.data(META_ROLE) or ""
        if meta:
            meta_font = _font(option.font, 10)
            meta_w = QFontMetrics(meta_font).horizontalAdvance(meta)
            painter.setFont(meta_font)
            painter.setPen(css_color(tokens.text_muted))
            painter.drawText(
                QRect(right - meta_w, inner.top(), meta_w, inner.height()),
                Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignRight, meta,
            )
            right -= meta_w + dialog_scaled(10)

        text_font = _font(option.font, 13)
        text_w = max(0, right - x)
        painter.setFont(text_font)
        painter.setPen(css_color(tokens.text))
        text = QFontMetrics(text_font).elidedText(
            index.data(Qt.ItemDataRole.DisplayRole) or "", Qt.TextElideMode.ElideRight, text_w
        )
        painter.drawText(
            QRect(x, inner.top(), text_w, inner.height()),
            Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignLeft, text,
        )
        painter.restore()


def clear_thumbnail_cache():
    _thumbnail.cache_clear()


@lru_cache(maxsize=256)
def _thumbnail(data_url: str) -> Optional[QPixmap]:
    """图片条目的缩略图（data URL），解码结果按 URL 缓存。"""
    if not data_url.startswith("data:image"):
        return None
    try:
        pixmap = QPixmap()
        pixmap.loadFromData(base64.b64decode(data_url.split(",", 1)[1]))
    except (ValueError, IndexError):
        return None
    return None if pixmap.isNull() else pixmap
