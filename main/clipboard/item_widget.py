# -*- coding: utf-8 -*-
"""
ClipboardItemWidget — 剪贴板列表项显示组件

负责单条剪贴板记录的渲染。
"""

from typing import Optional

from PySide6.QtWidgets import QFrame, QVBoxLayout, QHBoxLayout, QLabel, QSizePolicy
from PySide6.QtCore import Qt, Signal
from PySide6.QtGui import QPixmap, QCursor

from .data_manager import ClipboardItem
from .preview_popup import PreviewPopup
from .themes import get_theme_manager, Theme
from .theme_styles import ThemeStyleGenerator
from core.logger import log_error
from core import safe_event


class ClipboardItemWidget(QFrame):
    """剪贴板项显示组件"""

    clicked = Signal(int)         # 点击信号，传递 item_id
    double_clicked = Signal(int)  # 双击信号
    hover_entered = Signal(int)   # 鼠标进入信号
    hover_left = Signal(int)      # 鼠标离开信号

    # ---- 样式缓存（类级别，避免每个 widget 都拼接/解析 CSS） ----
    _style_cache: dict = {}   # key: (theme_name, opacity, font_size, is_selected, is_odd)

    @classmethod
    def get_cached_style(cls, theme: Theme, opacity: int, font_size: int,
                         is_selected: bool, row_index: int) -> str:
        """获取缓存的样式字符串，命中率极高（只有4种组合）"""
        is_odd = row_index % 2 == 1
        key = (id(theme), opacity, font_size, is_selected, is_odd)
        style = cls._style_cache.get(key)
        if style is None:
            generator = ThemeStyleGenerator(theme)
            style = generator.generate_item_widget_style(
                is_selected, 1 if is_odd else 0, opacity, font_size
            )
            cls._style_cache[key] = style
        return style

    @classmethod
    def invalidate_style_cache(cls):
        """主题/透明度变化时清空缓存"""
        cls._style_cache.clear()

    def __init__(
        self,
        item: ClipboardItem,
        display_lines: int = 1,
        row_index: int = 0,
        window_opacity: int = 0,
        show_metadata: bool = True,
        theme: Theme = None,
        line_height_padding: int = 8,
        parent=None,
    ):
        super().__init__(parent)
        self.item = item
        self.display_lines = display_lines
        self.row_index = row_index
        self.window_opacity = window_opacity
        self.show_metadata = show_metadata
        self._is_selected = False
        self.line_height_padding = line_height_padding

        # 主题支持
        if theme is None:
            self.theme = get_theme_manager().get_current_theme()
        else:
            self.theme = theme

        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        self._setup_ui()

    def _setup_ui(self):
        """设置 UI"""
        self.setFrameShape(QFrame.Shape.NoFrame)

        # 通过 _update_style() 统一设置所有样式（容器 + 子控件）
        self._update_style()

        layout = QVBoxLayout(self)
        layout.setContentsMargins(12, 1, 5, 1)
        layout.setSpacing(0)

        # 第一行：图标/缩略图 + 内容预览
        top_layout = QHBoxLayout()

        if self.item.content_type == "image" and self.item.thumbnail:
            thumbnail_label = QLabel()
            thumbnail_label.setObjectName("iconLabel")
            pixmap = self._load_thumbnail(self.item.thumbnail)
            if pixmap:
                thumbnail_label.setPixmap(pixmap)
                thumbnail_label.setFixedSize(40, 40)
                thumbnail_label.setScaledContents(True)
                top_layout.addWidget(thumbnail_label)
        elif self.item.icon:
            icon_label = QLabel(self.item.icon)
            icon_label.setObjectName("iconLabel")
            icon_label.setFixedWidth(24)
            top_layout.addWidget(icon_label)

        # 内容预览
        content_label = QLabel(self.item.display_text)
        content_label.setObjectName("contentLabel")
        content_label.setTextFormat(Qt.TextFormat.PlainText)
        content_label.setWordWrap(False)

        font_metrics = content_label.fontMetrics()
        line_height = font_metrics.height()
        content_label.setMinimumHeight(line_height)
        content_label.setTextInteractionFlags(Qt.TextInteractionFlag.NoTextInteraction)
        content_label.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Preferred)
        content_label.setMinimumWidth(0)
        top_layout.addWidget(content_label, 1)

        # 置顶标记
        if self.item.is_pinned:
            pin_label = QLabel("📌")
            pin_label.setObjectName("iconLabel")
            top_layout.addWidget(pin_label)

        layout.addLayout(top_layout)

        # 第二行：来源应用 + 时间
        if self.show_metadata:
            bottom_layout = QHBoxLayout()
            bottom_layout.setContentsMargins(0, 0, 0, 0)
            bottom_layout.addStretch()

            if self.item.source_app:
                source_label = QLabel(self.item.source_app)
                source_label.setObjectName("metaLabel")
                source_label.setContentsMargins(0, 0, 0, 0)
                bottom_layout.addWidget(source_label)

            if self.item.created_at:
                if self.item.source_app:
                    separator_label = QLabel(" · ")
                    separator_label.setObjectName("metaLabel")
                    bottom_layout.addWidget(separator_label)

                time_label = QLabel(self.item.created_at.strftime("%m-%d %H:%M"))
                time_label.setObjectName("metaLabel")
                time_label.setContentsMargins(0, 0, 0, 0)
                bottom_layout.addWidget(time_label)

            layout.addLayout(bottom_layout)

    def _load_thumbnail(self, data_url: str) -> Optional[QPixmap]:
        """从 Base64 Data URL 加载缩略图"""
        try:
            import base64
            if data_url.startswith("data:image"):
                header, data = data_url.split(",", 1)
                image_data = base64.b64decode(data)
                pixmap = QPixmap()
                pixmap.loadFromData(image_data)
                return pixmap
        except Exception as e:
            log_error(f"加载缩略图失败: {e}", "Clipboard")
        return None

    def set_selected(self, selected: bool):
        """设置选中状态并更新样式"""
        if self._is_selected == selected:
            return
        self._is_selected = selected
        self._update_style()

    def _update_style(self):
        """更新样式（根据选中状态和主题），使用缓存避免重复生成"""
        font_size = self.display_lines if self.display_lines >= 10 else 15
        style = ClipboardItemWidget.get_cached_style(
            self.theme, self.window_opacity, font_size,
            self._is_selected, self.row_index
        )
        self.setStyleSheet(style)

    @safe_event
    def mousePressEvent(self, event):
        """鼠标点击 - 选择该项"""
        if event.button() == Qt.MouseButton.LeftButton:
            self.clicked.emit(self.item.id)
        super().mousePressEvent(event)

    @safe_event
    def mouseDoubleClickEvent(self, event):
        """鼠标双击 - 禁用"""
        pass

    @safe_event
    def enterEvent(self, event):
        """鼠标进入 - 通知高亮并触发预览"""
        super().enterEvent(event)
        self.hover_entered.emit(self.item.id)
        popup = PreviewPopup.instance()
        pos = QCursor.pos()
        popup.show_preview(self.item, pos, delay_ms=500)

    @safe_event
    def leaveEvent(self, event):
        """鼠标离开 - 通知取消高亮并隐藏预览"""
        super().leaveEvent(event)
        self.hover_left.emit(self.item.id)
        PreviewPopup.instance().hide_preview()
 