# -*- coding: utf-8 -*-
"""
剪贴板悬停预览弹窗

提供 HTML 富文本和图片的悬停预览功能。
"""

from PySide6.QtWidgets import (
    QWidget, QVBoxLayout, QLabel, QTextEdit, QApplication
)
from PySide6.QtCore import Qt, QPoint, QTimer
from PySide6.QtGui import QPixmap

from core.logger import log_exception
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from .data_manager import ClipboardManager, ClipboardItem


class PreviewPopup(QWidget):
    """悬停预览弹窗 - 支持 HTML 富文本和图片预览"""
    
    _instance = None  # 单例，避免多个弹窗
    
    @classmethod
    def instance(cls):
        if cls._instance is None:
            cls._instance = cls()
        return cls._instance
    
    def __init__(self):
        super().__init__(None)
        # 无边框 + 工具窗口 + 置顶
        self.setWindowFlags(
            Qt.WindowType.ToolTip |
            Qt.WindowType.FramelessWindowHint |
            Qt.WindowType.WindowStaysOnTopHint
        )
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, False)
        self.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating, True)
        
        self.setStyleSheet("""
            PreviewPopup {
                background: #FAFAFA;
                border: 1px solid #D0D0D0;
                border-radius: 8px;
            }
        """)
        
        self._setup_ui()
        self._manager = None
        self._current_item_id = None
        
        # 延迟显示定时器
        self._show_timer = QTimer()
        self._show_timer.setSingleShot(True)
        self._show_timer.timeout.connect(self._do_show)
        self._pending_item = None
        self._pending_pos = None
        self._pending_prefer_side = "auto"
        self._pending_avoid_rect = None
    
    def _setup_ui(self):
        layout = QVBoxLayout(self)
        layout.setContentsMargins(2, 2, 2, 2)
        layout.setSpacing(0)
        
        # 标题行（仅用于文本预览，图片预览时隐藏）
        self.title_label = QLabel()
        self.title_label.setAlignment(Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignTop)
        self.title_label.setStyleSheet("font-size: 12px; color: #666; font-weight: bold; padding-left: 4px;")
        self.title_label.hide()  # 默认隐藏
        layout.addWidget(self.title_label)
        
        # 内容区域 - 使用 QTextEdit 支持富文本
        self.content_widget = QTextEdit()
        self.content_widget.setReadOnly(True)
        # 启用自动换行
        self.content_widget.setLineWrapMode(QTextEdit.LineWrapMode.WidgetWidth)
        # 禁用滚动条
        self.content_widget.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.content_widget.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.content_widget.setStyleSheet("""
            QTextEdit {
                background: #FAFAFA;
                border: 1px solid #E0E0E0;
                border-radius: 4px;
                padding: 4px 8px;
                font-size: 13px;
                color: #333;
            }
        """)
        # 设置文档边距为0，减少额外空间
        self.content_widget.document().setDocumentMargin(2)
        # 只设置最大尺寸，让内容自适应
        self.content_widget.setMaximumSize(500, 400)
        layout.addWidget(self.content_widget)
        
        # 图片预览（默认隐藏）
        self.image_label = QLabel()
        self.image_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.image_label.setStyleSheet("background: #F0F0F0; border: 1px solid #E0E0E0; border-radius: 4px;")
        self.image_label.hide()
        layout.addWidget(self.image_label)
    
    def set_manager(self, manager: 'ClipboardManager'):
        """设置剪贴板管理器（用于加载图片）"""
        self._manager = manager
    
    def show_preview(self, item: 'ClipboardItem', pos: QPoint, delay_ms: int = 5, prefer_side: str = "auto", avoid_rect=None):
        """
        显示预览（带延迟）
        
        Args:
            item: 剪贴板项
            pos: 显示位置
            delay_ms: 延迟毫秒数，0 表示立即显示
            prefer_side: 预览优先显示方向（"left" | "right" | "auto"）
            avoid_rect: 需要避开的矩形区域（通常为主窗口矩形）
        """
        if not item:
            self.hide_preview()
            return
        
        # 如果是同一个项目且已显示，忽略
        if self._current_item_id == item.id and self.isVisible():
            return
        
        self._pending_item = item
        self._pending_pos = pos
        self._pending_prefer_side = prefer_side
        self._pending_avoid_rect = avoid_rect
        
        if delay_ms > 0:
            self._show_timer.start(delay_ms)
        else:
            self._do_show()
    
    def _do_show(self):
        """实际执行显示"""
        item = self._pending_item
        pos = self._pending_pos
        
        if not item:
            return
        
        self._current_item_id = item.id
        
        # 根据内容类型显示不同预览
        if item.content_type == "image":
            self._show_image_preview(item)
        elif item.content_type == "file":
            self._show_file_preview(item)
        elif item.content_type == "text":
            # 文本类型也显示预览
            self._show_text_preview(item)
        else:
            # 其他类型不显示预览
            return
        
        # 调整位置（在触发位置右侧显示）
        self.adjustSize()

        screen = QApplication.screenAt(pos) or QApplication.primaryScreen()
        screen_geo = screen.availableGeometry() if screen else QApplication.primaryScreen().availableGeometry()

        gap = 10
        x = pos.x() + 20
        y = pos.y()

        avoid_rect = self._pending_avoid_rect
        prefer_side = self._pending_prefer_side or "auto"

        if avoid_rect is not None:
            left_space = avoid_rect.left() - screen_geo.left()
            right_space = screen_geo.right() - avoid_rect.right()

            def place_left():
                return avoid_rect.left() - self.width() - gap

            def place_right():
                return avoid_rect.right() + gap

            if prefer_side == "left":
                if left_space >= self.width() + gap:
                    x = place_left()
                elif right_space >= self.width() + gap:
                    x = place_right()
                else:
                    x = place_right() if right_space >= left_space else place_left()
            elif prefer_side == "right":
                if right_space >= self.width() + gap:
                    x = place_right()
                elif left_space >= self.width() + gap:
                    x = place_left()
                else:
                    x = place_left() if left_space >= right_space else place_right()
            else:
                # auto: 优先右侧，不够再左侧
                if right_space >= self.width() + gap:
                    x = place_right()
                elif left_space >= self.width() + gap:
                    x = place_left()
                else:
                    x = place_right() if right_space >= left_space else place_left()

        # 确保不超出屏幕
        if x + self.width() > screen_geo.right():
            x = screen_geo.right() - self.width() - gap
        if x < screen_geo.left():
            x = screen_geo.left()
        if y + self.height() > screen_geo.bottom():
            y = screen_geo.bottom() - self.height() - gap
        if y < screen_geo.top():
            y = screen_geo.top()

        self.move(x, y)
        self.show()
    
    def _show_text_preview(self, item: 'ClipboardItem'):
        """显示纯文本预览 - 上面显示时间，下面显示完整内容"""
        # 显示时间标题
        if item.created_at:
            time_str = item.created_at.strftime("%Y-%m-%d %H:%M:%S")
            self.title_label.setText(time_str)
        else:
            self.title_label.setText("📝 文本")
        self.title_label.show()
        
        # 显示完整内容（限制长度避免卡顿）
        content = item.content[:2000] if len(item.content) > 2000 else item.content
        
        # 先重置尺寸约束
        self.content_widget.setMinimumSize(0, 0)
        self.content_widget.setMaximumSize(500, 400)
        
        self.content_widget.setPlainText(content)
        
        # 根据内容调整大小
        self._adjust_content_size()
        
        self.content_widget.show()
        self.image_label.hide()
    
    def _show_file_preview(self, item: 'ClipboardItem'):
        """显示文件预览 - 文件名和完整路径"""
        import json
        import os
        from collections import defaultdict
        
        self.title_label.hide()  # 不显示标题
        self.image_label.hide()
        
        try:
            data = json.loads(item.content)
            files = data.get("files", [])
            
            if not files:
                self.content_widget.setPlainText("无文件信息")
                self.content_widget.show()
                return
            
            # 构建显示内容
            lines = []
            
            if len(files) == 1:
                # 单个文件
                filename = os.path.basename(files[0])
                lines.append(filename)
                lines.append("")
                lines.append(files[0])
            else:
                # 多个文件 - 按目录分组
                dir_files = defaultdict(list)
                for filepath in files:
                    dir_path = os.path.dirname(filepath)
                    filename = os.path.basename(filepath)
                    dir_files[dir_path].append(filename)
                
                if len(dir_files) == 1:
                    # 全部在同一目录
                    dir_path = list(dir_files.keys())[0]
                    filenames = dir_files[dir_path]
                    for fn in filenames:
                        lines.append(fn)
                    lines.append("")
                    lines.append(dir_path)
                else:
                    # 多个目录，按目录分组显示
                    group_num = 1
                    for dir_path, filenames in dir_files.items():
                        if len(filenames) == 1:
                            # 该目录只有一个文件
                            lines.append(f"[{group_num}] {filenames[0]}")
                            lines.append(os.path.join(dir_path, filenames[0]))
                        else:
                            # 该目录有多个文件
                            lines.append(f"[{group_num}]")
                            for fn in filenames:
                                lines.append(f"  {fn}")
                            lines.append(dir_path)
                        group_num += 1
                        lines.append("")  # 空行分隔
                    
                    # 移除最后的空行
                    if lines and lines[-1] == "":
                        lines.pop()
            
            text = "\n".join(lines)
            
            # 先重置尺寸约束
            self.content_widget.setMinimumSize(0, 0)
            self.content_widget.setMaximumSize(500, 400)
            
            self.content_widget.setPlainText(text)
            
            # 根据内容自适应大小
            self._adjust_content_size()
            self.content_widget.show()
            
        except Exception as e:
            log_exception(e, "加载文本预览")
            self.content_widget.setMinimumSize(0, 0)
            self.content_widget.setMaximumSize(500, 400)
            self.content_widget.setPlainText(item.content)
            self._adjust_content_size()
            self.content_widget.show()
    
    def _adjust_content_size(self):
        """根据内容调整 content_widget 大小 - 智能自适应宽度和高度"""
        doc = self.content_widget.document()
        
        # 获取字体度量
        font_metrics = self.content_widget.fontMetrics()
        line_height = font_metrics.height()
        
        # 获取纯文本内容
        text = doc.toPlainText()
        lines = text.split('\n')
        line_count = len(lines)
        
        # 计算最长行的宽度（不换行情况下的理想宽度）
        max_line_width = 0
        for line in lines:
            line_width = font_metrics.horizontalAdvance(line)
            max_line_width = max(max_line_width, line_width)
        
        # padding 计算：CSS padding 4px 上下 + 8px 左右 + 边框 2px + 文档边距 2px
        h_padding = 8 * 2 + 2 * 2 + 4  # 左右 padding + 边框 + 余量
        v_padding = 4 * 2 + 2 * 2 + 4  # 上下 padding + 边框 + 文档边距
        
        ideal_width = max_line_width + h_padding
        
        # 限制最大宽度
        max_width = 500
        min_width = 80
        
        if ideal_width <= max_width:
            # 内容不需要换行，使用理想宽度
            actual_width = max(ideal_width, min_width)
            # 不设置文档宽度限制，保持自然布局
            doc.setTextWidth(-1)
            # 高度基于实际行数
            actual_height = line_count * line_height + v_padding
        else:
            # 内容需要换行，使用最大宽度
            actual_width = max_width
            # 设置文档宽度让其自动换行
            doc.setTextWidth(max_width - h_padding)
            # 重新获取文档高度（换行后）
            actual_height = int(doc.size().height()) + v_padding
        
        # 限制高度范围
        max_height = 550
        min_height = line_height + v_padding  # 至少能显示一行
        
        actual_height = min(actual_height, max_height)
        actual_height = max(actual_height, min_height)
        
        # 设置固定尺寸
        self.content_widget.setFixedSize(int(actual_width), int(actual_height))
    
    def _show_html_preview(self, item: 'ClipboardItem'):
        """显示 HTML 富文本预览"""
        self.title_label.setText("富文本预览")
        html = item.html_content
        
        if html:
            # 限制大小，避免过大的 HTML 卡顿
            if len(html) > 50000:
                html = html[:50000] + "..."
            self.content_widget.setHtml(html)
        else:
            self.content_widget.setPlainText(item.content[:2000])
        self.content_widget.show()
        self.image_label.hide()
    
    def _show_image_preview(self, item: 'ClipboardItem'):
        """显示图片预览"""
        self.title_label.hide()  # 图片预览不显示标题
        self.content_widget.hide()
        
        # 尝试加载完整图片
        if self._manager and item.image_id:
            image_data = self._manager.get_image_data(item.image_id)
            if image_data:
                # 确保是 bytes 类型（Rust 可能返回 list）
                if isinstance(image_data, list):
                    image_data = bytes(image_data)
                pixmap = QPixmap()
                pixmap.loadFromData(image_data)
                # 缩放到合适大小
                scaled = pixmap.scaled(
                    400, 300,
                    Qt.AspectRatioMode.KeepAspectRatio,
                    Qt.TransformationMode.SmoothTransformation
                )
                self.image_label.setPixmap(scaled)
                self.image_label.setFixedSize(scaled.size())
                self.image_label.show()
                return
        
        # Fallback: 使用缩略图
        if item.thumbnail:
            import base64
            try:
                if item.thumbnail.startswith("data:image"):
                    _, data = item.thumbnail.split(",", 1)
                    image_data = base64.b64decode(data)
                    pixmap = QPixmap()
                    pixmap.loadFromData(image_data)
                    scaled = pixmap.scaled(
                        400, 300,
                        Qt.AspectRatioMode.KeepAspectRatio,
                        Qt.TransformationMode.SmoothTransformation
                    )
                    self.image_label.setPixmap(scaled)
                    self.image_label.setFixedSize(scaled.size())
                    self.image_label.show()
            except Exception as e:
                log_exception(e, "加载图片预览")
    
    def hide_preview(self):
        """隐藏预览"""
        self._show_timer.stop()
        self._pending_item = None
        self._pending_pos = None
        self._pending_prefer_side = "auto"
        self._pending_avoid_rect = None
        self._current_item_id = None
        self.hide()
 