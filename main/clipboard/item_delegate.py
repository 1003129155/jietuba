# -*- coding: utf-8 -*-
"""
ClipboardItemDelegate — 剪贴板列表项绘制代理
"""

import base64
from typing import Optional, Dict, Tuple

from PySide6.QtWidgets import QStyledItemDelegate, QStyleOptionViewItem, QListWidget
from PySide6.QtCore import Qt, QRect, QSize, QModelIndex, QPoint
from PySide6.QtGui import (
    QPainter, QColor, QFont, QFontMetrics, QPixmap, QPen,
)

from .data_manager import ClipboardItem
from .themes import Theme, ThemeColors
from core.logger import log_error


# QListWidgetItem 自定义数据角色
ROLE_ITEM_DATA = Qt.ItemDataRole.UserRole + 1      # ClipboardItem 对象
ROLE_ITEM_ID = Qt.ItemDataRole.UserRole             # item.id（保持兼容）

# 前 35 个项目的快捷键标签（1-9, a-z），带全角冒号
_SHORTCUT_LABELS = [str(i) + '：' for i in range(1, 10)] + [chr(c) + '：' for c in range(ord('a'), ord('z') + 1)]
# 快捷键标签区域宽度（含冒号需要更宽）
_SHORTCUT_BADGE_WIDTH = 26


def _hex_to_qcolor(hex_color: str, alpha: int = 255) -> QColor:
    """将 '#RRGGBB' 转为 QColor"""
    hex_color = hex_color.lstrip('#')
    if len(hex_color) == 3:
        hex_color = ''.join(c * 2 for c in hex_color)
    r, g, b = int(hex_color[0:2], 16), int(hex_color[2:4], 16), int(hex_color[4:6], 16)
    return QColor(r, g, b, alpha)


def _parse_rgba(rgba_str: str) -> QColor:
    """解析 'rgba(r, g, b, a)' 格式为 QColor"""
    try:
        inner = rgba_str.strip().removeprefix("rgba(").removesuffix(")")
        parts = [p.strip() for p in inner.split(",")]
        r, g, b = int(parts[0]), int(parts[1]), int(parts[2])
        a = float(parts[3])
        return QColor(r, g, b, int(a * 255))
    except Exception:
        return QColor(0, 0, 0, 40)


class ClipboardItemDelegate(QStyledItemDelegate):
    """
    高性能列表项绘制代理

    所有视觉效果通过 QPainter 直接绘制，无 QWidget 创建开销。
    """

    # 缩略图缓存 (data_url -> QPixmap)
    _thumb_cache: Dict[str, Optional[QPixmap]] = {}

    def __init__(
        self,
        parent: QListWidget = None,
        theme: Theme = None,
        display_lines: int = 1,
        window_opacity: int = 0,
        show_metadata: bool = True,
        line_height_padding: int = 8,
        show_shortcuts: bool = True,
    ):
        super().__init__(parent)
        self._theme: Theme = theme
        self._display_lines = display_lines
        self._window_opacity = window_opacity
        self._show_metadata = show_metadata
        self._line_height_padding = line_height_padding
        self._show_shortcuts = show_shortcuts

        # 当前高亮的 item_id
        self._highlighted_id: Optional[int] = None

        # 预计算颜色缓存（主题变化时重建）
        self._colors_cache: Dict = {}
        if theme:
            self._rebuild_color_cache()

        # 字体缓存（避免 paint 中重复创建 QFont）
        self._font_cache: Dict[str, QFont] = {}
        self._fm_cache: Dict[str, QFontMetrics] = {}
        self._rebuild_font_cache()

    # ========== 配置更新 ==========

    def set_theme(self, theme: Theme):
        self._theme = theme
        self._rebuild_color_cache()

    def set_display_lines(self, lines: int):
        self._display_lines = lines
        self._rebuild_font_cache()

    def set_window_opacity(self, opacity: int):
        self._window_opacity = opacity
        self._rebuild_color_cache()

    def set_show_metadata(self, show: bool):
        self._show_metadata = show

    def set_show_shortcuts(self, show: bool):
        self._show_shortcuts = show

    def set_highlighted_id(self, item_id: Optional[int]):
        self._highlighted_id = item_id

    def _rebuild_color_cache(self):
        """根据当前主题 + 透明度预计算所有需要的 QColor"""
        if not self._theme:
            return
        c = self._theme.colors
        alpha = 255 - int(self._window_opacity * 2.55)

        self._colors_cache = {
            "bg_even": _hex_to_qcolor(c.bg_primary, alpha),
            "bg_odd": _hex_to_qcolor(c.bg_secondary, alpha),
            "bg_selected": _parse_rgba(c.bg_selected_highlight),
            "border_bottom": _hex_to_qcolor(c.border_primary, 128),
            "border_selected": _hex_to_qcolor(c.border_selected),
            "text_primary": _hex_to_qcolor(c.text_primary),
            "text_tertiary": _hex_to_qcolor(c.text_tertiary),
            "shortcut_text": _hex_to_qcolor(c.shortcut_key_color),
        }

    @classmethod
    def clear_thumb_cache(cls):
        cls._thumb_cache.clear()

    def _rebuild_font_cache(self):
        """预创建 paint 中需要的所有 QFont 和 QFontMetrics"""
        font_size = self._display_lines if self._display_lines >= 10 else 15

        # 快捷键字体
        sf = QFont()
        sf.setPixelSize(max(10, font_size - 1))
        sf.setBold(True)
        self._font_cache["shortcut"] = sf
        self._fm_cache["shortcut"] = QFontMetrics(sf)

        # 图标字体
        icf = QFont()
        icf.setPixelSize(16)
        self._font_cache["icon"] = icf

        # 内容字体
        cf = QFont()
        cf.setPixelSize(font_size)
        self._font_cache["content"] = cf
        self._fm_cache["content"] = QFontMetrics(cf)

        # 元数据字体
        mf = QFont()
        mf.setPixelSize(10)
        self._font_cache["meta"] = mf

    # ========== 核心绘制 ==========

    def paint(self, painter: QPainter, option: QStyleOptionViewItem, index: QModelIndex):
        painter.save()
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, False)

        rect = option.rect
        item_data: ClipboardItem = index.data(ROLE_ITEM_DATA)
        item_id = index.data(ROLE_ITEM_ID)
        row = index.row()

        if not item_data:
            painter.restore()
            return

        is_selected = (item_id == self._highlighted_id) if self._highlighted_id is not None else False
        cc = self._colors_cache

        # ---- 背景 ----
        if is_selected:
            painter.fillRect(rect, cc["bg_selected"])
        elif row % 2 == 0:
            painter.fillRect(rect, cc["bg_even"])
        else:
            painter.fillRect(rect, cc["bg_odd"])

        # ---- 左边框（选中指示条） ----
        if is_selected:
            painter.fillRect(QRect(rect.left(), rect.top(), 3, rect.height()), cc["border_selected"])

        # ---- 底部分隔线 ----
        pen = QPen(cc["border_bottom"])
        pen.setWidth(1)
        painter.setPen(pen)
        painter.drawLine(rect.left(), rect.bottom(), rect.right(), rect.bottom())

        # ---- 快捷键徽标（前35项，左侧固定区域） ----
        shortcut_badge_width = _SHORTCUT_BADGE_WIDTH if self._show_shortcuts else 0
        if self._show_shortcuts and row < len(_SHORTCUT_LABELS):
            label = _SHORTCUT_LABELS[row]
            shortcut_font = self._font_cache["shortcut"]
            painter.setFont(shortcut_font)
            painter.setPen(cc["shortcut_text"])
            # 用 QFontMetrics 精确居中：从选中指示条(3px)之后的区域水平居中
            fm_s = self._fm_cache["shortcut"]
            text_w = fm_s.horizontalAdvance(label)
            badge_area_left = rect.left() + 3  # 跳过左侧选中指示条
            badge_x = badge_area_left + (shortcut_badge_width - text_w) // 2
            badge_y = rect.top() + (rect.height() + fm_s.ascent() - fm_s.descent()) // 2
            painter.drawText(badge_x, badge_y, label)

        # ---- 内容区域 ----
        # 有快捷键徽标时：徽标宽度 + 4px 间距；无徽标时：标准左边距 12px
        if self._show_shortcuts and row < len(_SHORTCUT_LABELS):
            content_left = rect.left() + shortcut_badge_width + 4
        else:
            content_left = rect.left() + 12
        content_right = rect.right() - 5  # 右边距
        content_top = rect.top() + 1
        content_width = content_right - content_left

        font_size = self._display_lines if self._display_lines >= 10 else 15
        x_offset = content_left

        # ---- 图标 / 缩略图 ----
        if item_data.content_type == "image" and item_data.thumbnail:
            pixmap = self._get_thumbnail(item_data.thumbnail)
            if pixmap:
                thumb_rect = QRect(x_offset, content_top + 1, 40, 40)
                painter.drawPixmap(thumb_rect, pixmap)
                x_offset += 44  # 40 + 4间距
        elif item_data.icon:
            painter.setFont(self._font_cache["icon"])
            painter.setPen(cc["text_primary"])
            icon_rect = QRect(x_offset, content_top, 24, int(font_size * 1.4))
            painter.drawText(icon_rect, Qt.AlignmentFlag.AlignVCenter, item_data.icon)
            x_offset += 24

        # ---- 内容文字 ----
        content_font = self._font_cache["content"]
        painter.setFont(content_font)
        painter.setPen(cc["text_primary"])

        text_rect = QRect(x_offset, content_top, content_right - x_offset, int(font_size * 1.4))

        # 如果有 标记，给右侧留空间
        pin_width = 0
        if item_data.is_pinned:
            pin_width = 24

        display_text = item_data.display_text
        fm = self._fm_cache["content"]
        elided_text = fm.elidedText(display_text, Qt.TextElideMode.ElideRight, text_rect.width() - pin_width)
        painter.drawText(text_rect, Qt.AlignmentFlag.AlignVCenter, elided_text)

        # ---- 置顶标记 ----
        if item_data.is_pinned:
            pin_rect = QRect(content_right - pin_width, content_top, pin_width, int(font_size * 1.4))
            painter.drawText(pin_rect, Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignRight, "📌")

        # ---- 第二行：元数据 ----
        if self._show_metadata:
            painter.setFont(self._font_cache["meta"])
            painter.setPen(cc["text_tertiary"])

            meta_parts = []
            if item_data.source_app:
                meta_parts.append(item_data.source_app)
            if item_data.created_at:
                meta_parts.append(item_data.created_at.strftime("%m-%d %H:%M"))
            meta_text = " · ".join(meta_parts)

            meta_y = content_top + int(font_size * 1.4)
            meta_rect = QRect(content_left, meta_y, content_width, 16)
            painter.drawText(meta_rect, Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter, meta_text)

        painter.restore()

    def sizeHint(self, option: QStyleOptionViewItem, index: QModelIndex) -> QSize:
        font_size = self._display_lines if self._display_lines >= 10 else 15
        content_height = int(font_size * 1.4) + 2  # 内容行 + 上下边距

        if self._show_metadata:
            content_height += 16  # 元数据行高

        item_data: ClipboardItem = index.data(ROLE_ITEM_DATA)
        if item_data and item_data.content_type == "image" and item_data.thumbnail:
            content_height = max(content_height, 42)  # 缩略图最小高度

        return QSize(option.rect.width() if option.rect.width() > 0 else 300, content_height)

    # ========== 辅助方法 ==========

    def _get_thumbnail(self, data_url: str) -> Optional[QPixmap]:
        """从缓存获取缩略图，未命中则解码并缓存"""
        if data_url in self._thumb_cache:
            return self._thumb_cache[data_url]

        try:
            if data_url.startswith("data:image"):
                _, data = data_url.split(",", 1)
                image_data = base64.b64decode(data)
                pixmap = QPixmap()
                pixmap.loadFromData(image_data)
                self._thumb_cache[data_url] = pixmap
                return pixmap
        except Exception as e:
            log_error(f"加载缩略图失败: {e}", "Clipboard")

        self._thumb_cache[data_url] = None
        return None
 