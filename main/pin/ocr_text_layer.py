# -*- coding: utf-8 -*-
"""
ocr_text_layer.py - OCR 可交互文字层（钉图专用）

在钉图窗口上叠加一个完全透明的文字选择层，支持：
- 鼠标悬停时显示文本选择光标
- 点击设置光标位置，拖拽选择连续文字（Word 风格）
- 支持钉图缩放时坐标自适应
- 绘画模式时自动禁用

使用：
当钉图生成后，自动异步触发 OCR 识别并创建此透明文字层
"""
from PyQt6.QtWidgets import QWidget, QApplication
from PyQt6.QtCore import Qt, QRect, QPoint, QRectF, pyqtSignal, QEvent
from PyQt6.QtGui import QPainter, QPen, QColor, QBrush, QCursor, QFont, QFontMetrics
from typing import List, Dict, Optional, Tuple
from core import log_info, log_debug
from core.logger import log_exception


class OCRTextItem:
    """OCR 识别的单个文字块"""
    
    def __init__(self, text: str, box: List[List[int]], score: float):
        """
        初始化文字块
        
        Args:
            text: 文字内容
            box: 四个角的坐标 [[x1,y1], [x2,y2], [x3,y3], [x4,y4]]（相对于原始图像）
            score: 识别置信度
        """
        self.text = text
        self.original_box = box  # 保存原始坐标
        self.score = score
        
        # 计算原始边界矩形（归一化坐标 0-1）
        xs = [p[0] for p in box]
        ys = [p[1] for p in box]
        self.norm_rect = QRectF(
            min(xs), min(ys),
            max(xs) - min(xs),
            max(ys) - min(ys)
        )
        
        # 用于文字内部字符定位
        self.char_positions: List[Tuple[int, int]] = []  # 每个字符的 x 位置（相对于文字块）
    
    def calculate_char_positions(self, rect: QRect):
        """计算每个字符的位置（均分）"""
        if not self.text:
            return
        
        char_count = len(self.text)
        char_width = rect.width() / char_count if char_count > 0 else 0
        
        self.char_positions.clear()
        for i in range(char_count + 1):  # +1 是为了包含结束位置
            x_pos = rect.x() + int(i * char_width)
            self.char_positions.append(x_pos)
    
    def get_char_index_at_pos(self, x: int, rect: QRect) -> int:
        """根据 x 坐标获取最接近的字符索引"""
        if not self.text or not self.char_positions:
            return 0
        
        # 确保 x 在文字块范围内（扩展检测范围）
        if x < rect.x():
            return 0  # 点击在左侧，返回起始位置
        if x > rect.x() + rect.width():
            return len(self.text)  # 点击在右侧，返回末尾位置
        
        # 找到最接近的字符位置
        for i, char_x in enumerate(self.char_positions):
            if x < char_x:
                # 判断是靠近前一个还是当前字符
                if i > 0:
                    prev_x = self.char_positions[i - 1]
                    mid_x = (prev_x + char_x) / 2
                    if x < mid_x:
                        return i - 1
                return i
        
        return len(self.text)  # 超出范围返回末尾
    
    def get_scaled_rect(self, scale_x: float, scale_y: float, original_width: int, original_height: int) -> QRect:
        """
        获取缩放后的矩形
        
        Args:
            scale_x: X轴缩放比例
            scale_y: Y轴缩放比例
            original_width: 原始图像宽度
            original_height: 原始图像高度
        """
        # 从归一化坐标转换为实际坐标
        x = int(self.norm_rect.x() * scale_x)
        y = int(self.norm_rect.y() * scale_y)
        w = int(self.norm_rect.width() * scale_x)
        h = int(self.norm_rect.height() * scale_y)
        return QRect(x, y, w, h)
    
    def contains(self, point: QPoint, scale_x: float, scale_y: float, original_width: int, original_height: int) -> bool:
        """检查点是否在缩放后的文字块内（扩大检测范围）"""
        rect = self.get_scaled_rect(scale_x, scale_y, original_width, original_height)
        # 扩大检测范围：上下左右各扩展5像素，提高点击容错率
        expanded_rect = rect.adjusted(-5, -5, 5, 5)
        return expanded_rect.contains(point)


class OCRTextLayer(QWidget):
    """OCR 可交互文字层（完全透明，Word 风格文字选择）"""
    
    def __init__(self, parent=None, original_width: int = 100, original_height: int = 100):
        super().__init__(parent)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        # 初始不透传，让鼠标事件能进入控件，然后在事件处理中判断是否需要透传
        self.setMouseTracking(True)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self._event_filter_target = None
        
        # 透传状态标志（避免频繁设置属性）
        self._is_transparent = False
        
        parent_widget = parent if isinstance(parent, QWidget) else None
        if parent_widget:
            parent_widget.installEventFilter(self)
            self._event_filter_target = parent_widget
            log_debug(f"已安装事件过滤器到父窗口: {parent_widget.__class__.__name__}", "OCR层")
            try:
                parent_widget.destroyed.connect(self._detach_event_filter)
            except Exception as e:
                log_exception(e, "连接destroyed信号")
        
        # 原始图像尺寸
        self.original_width = original_width
        self.original_height = original_height
        
        self.text_items: List[OCRTextItem] = []
        self.enabled = True  # 外部启用标志
        self.drawing_mode = False  # 绘图工具是否开启
        
        # Word 风格选择
        self.selection_start: Optional[Tuple[int, int]] = None  # (item_index, char_index)
        self.selection_end: Optional[Tuple[int, int]] = None    # (item_index, char_index)
        self.is_selecting = False
        
        # 双击检测
        self.last_click_time = 0
        self.last_click_pos: Optional[QPoint] = None
        
        # 当前鼠标是否在文字上
        self._mouse_on_text = False
        
        # 🚀 懒加载标志：字符位置是否已计算（优化加载性能）
        self._char_positions_calculated = False

    def _detach_event_filter(self):
        target = getattr(self, '_event_filter_target', None)
        if target:
            try:
                target.removeEventFilter(self)
            except Exception as e:
                log_exception(e, "移除事件过滤器")
        self._event_filter_target = None

    def _is_active(self) -> bool:
        """是否可用：外部启用且未处于绘图模式"""
        return self.enabled and not self.drawing_mode

    def set_drawing_mode(self, active: bool):
        """设置绘图模式开关，开启时屏蔽文字层交互"""
        self.drawing_mode = bool(active)
        self._apply_effective_enabled()

    def set_draw_tool_active(self, active: bool):
        """供工具栏按钮调用：按钮按下(True)/抬起(False) 即切换文字层。
        注意：这里代表工具处于"绘制工具被选中"的状态，而非实际开始绘制过程。
        """
        self.set_drawing_mode(active)

    def _apply_effective_enabled(self):
        """应用有效的启用状态：只有在启用且有文字块时才显示（优化版：延迟计算）"""
        if not self._is_active():
            # 禁用时清除选择
            self.clear_selection()
            self.setCursor(Qt.CursorShape.ArrowCursor)
            self.hide()
        else:
            # 启用时：检查是否有文字块
            if not self.text_items:
                self.hide()
                return
                
            # 🚀 优化：不立即计算字符位置，延迟到鼠标移动时
            # 有文字块时显示，接收所有鼠标事件（在事件处理中判断是否需要透传）
            # self.recalculate_char_positions()  # 注释掉立即计算
            self.raise_()
            self.show()
            
            # 确保事件过滤器已安装
            parent_widget = self.parentWidget()
            if parent_widget and self._event_filter_target != parent_widget:
                if self._event_filter_target:
                    self._event_filter_target.removeEventFilter(self)
                parent_widget.installEventFilter(self)
                self._event_filter_target = parent_widget
                log_debug(f"已安装事件过滤器到父窗口: {parent_widget.__class__.__name__}", "OCR层")

    def recalculate_char_positions(self):
        """
        根据当前尺寸重新计算所有文字块的字符位置（懒加载优化版）
        
        避免在加载时阻塞 UI，只在真正需要时才计算
        """
        if not self.text_items:
            return
        scale_x, scale_y = self.get_scale_factors()
        for item in self.text_items:
            rect = item.get_scaled_rect(scale_x, scale_y, self.original_width, self.original_height)
            item.calculate_char_positions(rect)
        
        # 标记已计算
        self._char_positions_calculated = True
    
    def _ensure_char_positions(self):
        """
        🚀 确保字符位置已计算（懒加载入口）
        
        只在首次鼠标交互时调用一次，避免加载时卡顿
        """
        if not self._char_positions_calculated and self.text_items:
            self.recalculate_char_positions()

    def _is_pos_on_text(self, pos: QPoint) -> bool:
        """给定本地坐标，判断是否在文字块扩展范围内"""
        scale_x, scale_y = self.get_scale_factors()
        for idx, item in enumerate(self.text_items):
            if item.contains(pos, scale_x, scale_y, self.original_width, self.original_height):
                return True
        return False
    
    def set_enabled(self, enabled: bool):
        """设置是否启用（绘画模式时设置为 False）"""
        self.enabled = enabled
        self._apply_effective_enabled()
    
    def load_ocr_result(self, ocr_result: Dict, original_width: int, original_height: int):
        """
        加载 OCR 识别结果（优化版：延迟计算，避免 UI 卡顿）
        
        Args:
            ocr_result: OCR 返回的字典格式结果
            original_width: 原始图像宽度
            original_height: 原始图像高度
        """
        self.text_items.clear()
        self.original_width = original_width
        self.original_height = original_height
        
        # 重置懒加载标志
        self._char_positions_calculated = False
        
        if ocr_result.get('code') != 100:
            return
        
        data = ocr_result.get('data', [])
        if not data:
            return
        
        for item in data:
            text = item.get('text', '')
            box = item.get('box', [])
            score = item.get('score', 0.0)
            
            # 明确检查 text 和 box 是否有效（避免 numpy 数组的真值判断问题）
            if text and box is not None and len(box) > 0:
                self.text_items.append(OCRTextItem(text, box, score))

        # OCR 模块已按阅读顺序排序，无需再次排序
        
        # 🚀 优化：不立即计算字符位置，延迟到真正需要时（鼠标交互时）
        # 这样可以避免加载时 UI 卡顿，让用户可以立即移动窗口
        # self.recalculate_char_positions()  # 注释掉立即计算
        
        # 加载完成后，如果已启用则显示文字层
        if self.enabled:
            self._apply_effective_enabled()
    
    def get_scale_factors(self) -> tuple:
        """获取当前缩放比例"""
        if self.original_width == 0 or self.original_height == 0:
            return (1.0, 1.0)
        
        scale_x = self.width() / self.original_width
        scale_y = self.height() / self.original_height
        return (scale_x, scale_y)
    
    def get_all_text(self, separator: str = "\n") -> str:
        """
        获取所有识别的文字（按阅读顺序拼接，同行合并）
        
        Args:
            separator: 行之间的分隔符，默认换行
            
        Returns:
            str: 所有识别的文字（同一行用空格连接，不同行用 separator 分隔）
        """
        if not self.text_items:
            return ""
        
        # 按行分组（使用行高容差判断是否同一行）
        if len(self.text_items) <= 1:
            return self.text_items[0].text if self.text_items else ""
        
        # 收集每个文字块的位置信息
        items_with_pos = []
        for item in self.text_items:
            center_y = item.norm_rect.y() + item.norm_rect.height() / 2
            height = item.norm_rect.height()
            items_with_pos.append({
                'item': item,
                'center_y': center_y,
                'height': height
            })
        
        # 计算行高容差
        avg_height = sum(b['height'] for b in items_with_pos) / len(items_with_pos)
        line_tolerance = avg_height * 0.5
        
        # 分行（文字块已按阅读顺序排列）
        lines = []
        current_line = []
        current_line_y = None
        
        for block in items_with_pos:
            if current_line_y is None:
                current_line = [block['item'].text]
                current_line_y = block['center_y']
            elif abs(block['center_y'] - current_line_y) <= line_tolerance:
                # 同一行，用空格连接
                current_line.append(block['item'].text)
            else:
                # 新的一行
                lines.append(" ".join(current_line))
                current_line = [block['item'].text]
                current_line_y = block['center_y']
        
        # 别忘了最后一行
        if current_line:
            lines.append(" ".join(current_line))
        
        return separator.join(lines)
    
    def has_text(self) -> bool:
        """检查是否有识别到的文字"""
        return bool(self.text_items)
    
    def clear_selection(self):
        """清除选择"""
        self.selection_start = None
        self.selection_end = None
        self.is_selecting = False
        self.update()
    
    def get_selected_text(self) -> str:
        """获取选中的文字"""
        if not self.selection_start or not self.selection_end:
            return ""
        
        start_item_idx, start_char_idx = self.selection_start
        end_item_idx, end_char_idx = self.selection_end
        
        # 确保 start 在 end 之前
        if start_item_idx > end_item_idx or (start_item_idx == end_item_idx and start_char_idx > end_char_idx):
            start_item_idx, end_item_idx = end_item_idx, start_item_idx
            start_char_idx, end_char_idx = end_char_idx, start_char_idx
        
        selected_parts = []
        
        for idx in range(start_item_idx, end_item_idx + 1):
            if idx >= len(self.text_items):
                break
            
            item = self.text_items[idx]
            
            if idx == start_item_idx and idx == end_item_idx:
                # 同一个文字块内选择
                selected_parts.append(item.text[start_char_idx:end_char_idx])
            elif idx == start_item_idx:
                # 起始文字块
                selected_parts.append(item.text[start_char_idx:])
            elif idx == end_item_idx:
                # 结束文字块
                selected_parts.append(item.text[:end_char_idx])
            else:
                # 中间的完整文字块
                selected_parts.append(item.text)
        
        return "".join(selected_parts)
    
    def paintEvent(self, event):
        """绘制选择高亮"""
        if not self._is_active() or not self.selection_start or not self.selection_end:
            return
        
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        
        # 绘制选择背景（半透明蓝色）
        selection_color = QColor(100, 150, 255, 100)
        painter.fillRect(self.rect(), Qt.GlobalColor.transparent)
        
        start_item_idx, start_char_idx = self.selection_start
        end_item_idx, end_char_idx = self.selection_end
        
        # 确保 start 在 end 之前
        if start_item_idx > end_item_idx or (start_item_idx == end_item_idx and start_char_idx > end_char_idx):
            start_item_idx, end_item_idx = end_item_idx, start_item_idx
            start_char_idx, end_char_idx = end_char_idx, start_char_idx
        
        scale_x, scale_y = self.get_scale_factors()
        
        # 绘制每个被选中的文字块区域
        for idx in range(start_item_idx, end_item_idx + 1):
            if idx >= len(self.text_items):
                break
            
            item = self.text_items[idx]
            rect = item.get_scaled_rect(scale_x, scale_y, self.original_width, self.original_height)
            
            if not item.char_positions or len(item.char_positions) == 0:
                continue
            
            # 计算选择区域的起始和结束 x 坐标
            if idx == start_item_idx and idx == end_item_idx:
                # 同一个文字块内
                x1 = item.char_positions[min(start_char_idx, len(item.char_positions) - 1)]
                x2 = item.char_positions[min(end_char_idx, len(item.char_positions) - 1)]
            elif idx == start_item_idx:
                # 起始文字块
                x1 = item.char_positions[min(start_char_idx, len(item.char_positions) - 1)]
                x2 = rect.x() + rect.width()
            elif idx == end_item_idx:
                # 结束文字块
                x1 = rect.x()
                x2 = item.char_positions[min(end_char_idx, len(item.char_positions) - 1)]
            else:
                # 中间的完整文字块
                x1 = rect.x()
                x2 = rect.x() + rect.width()
            
            # 绘制选择矩形
            selection_rect = QRect(x1, rect.y(), x2 - x1, rect.height())
            painter.fillRect(selection_rect, selection_color)
    
    def eventFilter(self, obj, event):
        """事件过滤器：保留用于特殊情况，但主要逻辑已移到直接的鼠标事件处理中"""
        # 主要的鼠标事件处理现在在 mousePressEvent/mouseMoveEvent 中
        # 这里只保留作为备用
        return False  # 不拦截，让事件继续传递
    
    def mousePressEvent(self, event):
        """鼠标按下事件 - Word 风格点击设置光标"""
        if not self._is_active() or event.button() != Qt.MouseButton.LeftButton:
            # 透传给父窗口
            event.ignore()
            return
        
        pos = event.pos()
        
        # 检查是否点击在父窗口的按钮上（关闭按钮、工具栏切换按钮等）
        if self.parent():
            # 检查关闭按钮
            if hasattr(self.parent(), 'close_button') and self.parent().close_button.isVisible():
                button_rect = self.parent().close_button.geometry()
                if button_rect.contains(pos):
                    event.ignore()  # 让按钮处理
                    return
            
            # 检查工具栏切换按钮
            if hasattr(self.parent(), 'toolbar_toggle_button') and self.parent().toolbar_toggle_button.isVisible():
                button_rect = self.parent().toolbar_toggle_button.geometry()
                if button_rect.contains(pos):
                    event.ignore()  # 让按钮处理
                    return
        
        # 检查是否点击在文字上
        item_idx, char_idx = self._get_char_at_pos(pos, strict=True)
        
        if item_idx is None:
            # 点击在空白处：清除选择并透传给父窗口（允许拖动钉图）
            if self.selection_start or self.selection_end:
                # 有选择时，第一次点击空白清除选择
                self.clear_selection()
            # 让事件继续传递给父窗口用于拖动
            event.ignore()
            return
        
        # 点击在文字上，处理选择逻辑
        event.accept()
        self.setFocus()
        
        # 检测双击
        import time
        current_time = time.time()
        is_double_click = False
        
        if self.last_click_pos and self.last_click_time:
            time_diff = current_time - self.last_click_time
            pos_diff = (pos - self.last_click_pos).manhattanLength()
            
            # 双击条件：500ms 内，距离小于 5 像素
            if time_diff < 0.5 and pos_diff < 5:
                is_double_click = True
        
        self.last_click_time = current_time
        self.last_click_pos = pos
        
        if is_double_click:
            # 双击：选择整个文字块并自动复制
            self._select_word(item_idx)
        else:
            # 单击：设置光标位置并开始选择
            self.selection_start = (item_idx, char_idx)
            self.selection_end = (item_idx, char_idx)
            self.is_selecting = True
        
        self.update()
    
    def mouseMoveEvent(self, event):
        """鼠标移动事件 - 动态切换光标 + 拖拽选择文字（懒加载优化）"""
        if not self._is_active():
            # 不活跃时停止选择并透传
            if self.is_selecting:
                self.is_selecting = False
            self.setCursor(Qt.CursorShape.ArrowCursor)
            event.ignore()
            return
        
        # 🚀 懒加载：首次鼠标移动时才计算字符位置
        self._ensure_char_positions()
        
        pos = event.pos()
        
        # 检查是否在父窗口的按钮上
        on_button = False
        if self.parent():
            # 检查关闭按钮
            if hasattr(self.parent(), 'close_button') and self.parent().close_button.isVisible():
                button_rect = self.parent().close_button.geometry()
                if button_rect.contains(pos):
                    on_button = True
            
            # 检查工具栏切换按钮
            if not on_button and hasattr(self.parent(), 'toolbar_toggle_button') and self.parent().toolbar_toggle_button.isVisible():
                button_rect = self.parent().toolbar_toggle_button.geometry()
                if button_rect.contains(pos):
                    on_button = True
        
        # 如果在按钮上，透传事件并使用普通光标
        if on_button:
            self.setCursor(Qt.CursorShape.ArrowCursor)
            event.ignore()
            return
        
        # 1. 处理拖拽选择
        if self.is_selecting and self.selection_start:
            # 拖拽时使用非严格模式，允许跨行选择
            item_idx, char_idx = self._get_char_at_pos(pos, strict=False)
            
            if item_idx is not None:
                self.selection_end = (item_idx, char_idx)
                self.update()
            
            event.accept()
            return

        # 动态切换光标
        on_text = self._is_pos_on_text(pos)
        
        if on_text:
            # 在文字上：显示文本光标，接受事件
            self.setCursor(Qt.CursorShape.IBeamCursor)
            self._mouse_on_text = True
            event.accept()
        else:
            # 不在文字上：使用普通光标，透传事件给父窗口
            self.setCursor(Qt.CursorShape.ArrowCursor)
            self._mouse_on_text = False
            event.ignore()
    
    def mouseReleaseEvent(self, event):
        """鼠标释放事件"""
        if not self._is_active() or event.button() != Qt.MouseButton.LeftButton:
            event.ignore()
            return
        
        if self.is_selecting:
            self.is_selecting = False
            event.accept()
        else:
            # 透传给父窗口
            event.ignore()
    
    def _get_char_at_pos(self, pos: QPoint, strict: bool = False) -> Tuple[Optional[int], Optional[int]]:
        """获取指定位置的字符索引，支持跨行拖拽：
        1) 命中文字块：返回该块的字符索引
        2) 不命中时：
           - strict=True: 返回 (None, None)
           - strict=False: 选取垂直距离最近的文字块，并计算对应字符位置（用于拖拽选择）
        """
        scale_x, scale_y = self.get_scale_factors()

        nearest_idx = None
        nearest_dy = None
        nearest_rect = None

        for item_idx, item in enumerate(self.text_items):
            rect = item.get_scaled_rect(scale_x, scale_y, self.original_width, self.original_height)

            # 使用扩展的检测范围
            expanded_rect = rect.adjusted(-5, -5, 5, 5)

            # 计算最近行
            dy = abs(pos.y() - rect.center().y())
            if nearest_dy is None or dy < nearest_dy:
                nearest_dy = dy
                nearest_idx = item_idx
                nearest_rect = rect

            if expanded_rect.contains(pos):
                if not item.char_positions:
                    item.calculate_char_positions(rect)
                char_idx = item.get_char_index_at_pos(pos.x(), rect)
                return (item_idx, char_idx)

        # 如果是严格模式，未命中则返回 None
        if strict:
            return (None, None)

        # 未命中任何块时，选择最近行
        if nearest_idx is not None and nearest_rect is not None:
            item = self.text_items[nearest_idx]
            if not item.char_positions:
                item.calculate_char_positions(nearest_rect)

            # x 超出时也要选择：左侧=开头，右侧=末尾
            char_idx = item.get_char_index_at_pos(pos.x(), nearest_rect)
            return (nearest_idx, char_idx)

        return (None, None)
    
    def _select_word(self, item_idx: int):
        """选择整个文字块（双击时）并自动复制"""
        if item_idx >= len(self.text_items):
            return
        
        item = self.text_items[item_idx]
        self.selection_start = (item_idx, 0)
        self.selection_end = (item_idx, len(item.text))
        self.is_selecting = False
        
        # 立即复制
        self._copy_selected_text()
    
    def _copy_selected_text(self):
        """复制选中的文字到剪贴板（Word 风格）"""
        if not self.selection_start or not self.selection_end:
            return
        
        # 标准化选择范围
        start_item, start_char = self.selection_start
        end_item, end_char = self.selection_end
        
        if start_item > end_item or (start_item == end_item and start_char > end_char):
            start_item, end_item = end_item, start_item
            start_char, end_char = end_char, start_char
        
        # 提取选中的文字
        selected_text_parts = []
        
        for item_idx in range(start_item, end_item + 1):
            if item_idx >= len(self.text_items):
                break
            
            item = self.text_items[item_idx]
            
            # 确定当前文字块的选择范围
            if item_idx == start_item and item_idx == end_item:
                # 同一个文字块
                selected_text_parts.append(item.text[start_char:end_char])
            elif item_idx == start_item:
                # 起始文字块
                selected_text_parts.append(item.text[start_char:])
            elif item_idx == end_item:
                # 结束文字块
                selected_text_parts.append(item.text[:end_char])
            else:
                # 中间的文字块，全选
                selected_text_parts.append(item.text)
        
        selected_text = ''.join(selected_text_parts)
        
        if selected_text:
            # 复制到剪贴板
            clipboard = QApplication.clipboard()
            clipboard.setText(selected_text)
            log_info(f"已复制: {selected_text[:50]}{'...' if len(selected_text) > 50 else ''}", module="OCR文字层")
    
    def keyPressEvent(self, event):
        """键盘事件"""
        if not self._is_active():
            # ⚠️ 关键：禁用时不处理事件，但要透传给父窗口
            event.ignore()
            return
        
        # Ctrl+C: 复制
        if event.modifiers() & Qt.KeyboardModifier.ControlModifier and event.key() == Qt.Key.Key_C:
            self._copy_selected_text()
            event.accept()
        # Ctrl+A: 全选所有文字
        elif event.modifiers() & Qt.KeyboardModifier.ControlModifier and event.key() == Qt.Key.Key_A:
            if self.text_items:
                self.selection_start = (0, 0)
                self.selection_end = (len(self.text_items) - 1, len(self.text_items[-1].text))
                self.update()
            event.accept()
        # Escape: 清除选择
        elif event.key() == Qt.Key.Key_Escape:
            if self.selection_start or self.selection_end:
                self.clear_selection()
                event.accept()
            else:
                # 没有选择时，透传给父窗口（允许关闭钉图）
                event.ignore()
        else:
            # 其他按键透传给父窗口
            event.ignore()
    
    def resizeEvent(self, event):
        """窗口缩放时重新计算字符位置（优化：仅标记需要重新计算）"""
        super().resizeEvent(event)
        # 🚀 优化：只标记需要重新计算，下次鼠标交互时才真正计算
        self._char_positions_calculated = False
        # 如果用户正在选择文字，立即重新计算，保证选择准确
        if self.is_selecting or self.selection_start:
            self.recalculate_char_positions()
    
    def cleanup(self):
        """清理资源"""
        # 移除事件过滤器
        self._detach_event_filter()
        
        # 清除文字块
        self.text_items.clear()
        
        # 重置懒加载标志
        self._char_positions_calculated = False
        self.clear_selection()
    
    def closeEvent(self, event):
        """窗口关闭时清理"""
        self.cleanup()
        super().closeEvent(event)
