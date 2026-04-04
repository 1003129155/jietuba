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
import unicodedata
from PySide6.QtWidgets import QWidget, QApplication
from PySide6.QtCore import Qt, QRect, QPoint, QRectF, Signal, QEvent
from PySide6.QtGui import QPainter, QPen, QColor, QBrush, QCursor, QFont, QFontMetrics
from typing import List, Dict, Optional, Tuple
from core import log_info, log_debug, safe_event
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
        
        # 计算原始边界矩形（像素坐标）
        xs = [p[0] for p in box]
        ys = [p[1] for p in box]
        self.norm_rect = QRectF(
            min(xs), min(ys),
            max(xs) - min(xs),
            max(ys) - min(ys)
        )
        # 保存原始 norm_rect，用于图像变换时重新映射
        self._orig_norm_rect = QRectF(self.norm_rect)
        
        # 用于文字内部字符定位
        self.char_positions: List[Tuple[int, int]] = []  # 每个字符的位置（沿主轴）
        self._is_vertical = False  # 是否为垂直排列（旋转后）
    
    def calculate_char_positions(self, rect: QRect):
        """计算每个字符的位置（按字符宽度比例分配，全角:半角 ≈ 2:1）"""
        if not self.text:
            return
        
        char_count = len(self.text)
        # 根据方向决定沿宽度还是高度分配字符
        self._is_vertical = rect.height() > rect.width() * 1.5
        
        # 按 Unicode 东亚宽度属性分配比例
        weights = []
        for ch in self.text:
            eaw = unicodedata.east_asian_width(ch)
            if eaw in ('W', 'F'):  # Wide / Fullwidth（CJK 汉字、全角标点等）
                weights.append(2.0)
            else:  # Halfwidth, Narrow, Neutral, Ambiguous（数字、字母、半角标点）
                weights.append(1.0)
        
        total_weight = sum(weights)
        if total_weight == 0:
            total_weight = 1.0
        
        if self._is_vertical:
            total_size = rect.height()
            start = rect.y()
        else:
            total_size = rect.width()
            start = rect.x()
        
        self.char_positions.clear()
        cumulative = 0.0
        for i in range(char_count):
            pos = start + int(cumulative / total_weight * total_size)
            self.char_positions.append(pos)
            cumulative += weights[i]
        # 末尾位置（最后一个字符的右/下边缘）
        self.char_positions.append(start + int(total_size))
    
    def get_char_index_at_pos(self, x: int, rect: QRect, y: int = 0) -> int:
        """根据坐标获取最接近的字符索引（自动判断水平/垂直）"""
        if not self.text or not self.char_positions:
            return 0
        
        # 垂直模式使用 y 坐标，水平模式使用 x 坐标
        coord = y if self._is_vertical else x
        if self._is_vertical:
            start = rect.y()
            end = rect.y() + rect.height()
        else:
            start = rect.x()
            end = rect.x() + rect.width()
        
        if coord < start:
            return 0
        if coord > end:
            return len(self.text)
        
        # 找到最接近的字符位置
        for i, char_pos in enumerate(self.char_positions):
            if coord < char_pos:
                if i > 0:
                    prev_pos = self.char_positions[i - 1]
                    mid = (prev_pos + char_pos) / 2
                    if coord < mid:
                        return i - 1
                return i
        
        return len(self.text)
    
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
        self.setMouseTracking(True)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self._event_filter_target = None
        self._text_union_rect: Optional[QRectF] = None  # OCR 文本块整体包围盒（原始坐标）
        
        # 不使用穿透模式，而是接收事件并通过ignore()传递
        # 这样可以检测鼠标是否在文字上，同时避免raise_()掉帧
        # self._is_mouse_transparent 用于跟踪鼠标状态，不再设置实际属性
        
        parent_widget = parent if isinstance(parent, QWidget) else None
        if parent_widget:
            try:
                parent_widget.destroyed.connect(self.cleanup)
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
        
        # 懒加载标志：字符位置是否已计算（优化加载性能）
        self._char_positions_calculated = False

    def _is_active(self) -> bool:
        """是否可用：外部启用且未处于绘图模式"""
        return self.enabled and not self.drawing_mode

    def _is_parent_dragging(self) -> bool:
        parent = self.parent()
        return bool(parent and getattr(parent, "_is_dragging", False))

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
        """应用有效的启用状态：启用时始终显示层（无文字时事件会自动穿透）"""
        if not self._is_active():
            # 禁用时清除选择
            self.clear_selection()
            self.setCursor(Qt.CursorShape.ArrowCursor)
            self.hide()
        else:
            # 无文字时 mouseMoveEvent/mousePressEvent 均会 event.ignore() 穿透，不影响交互
            if not self.isVisible():
                self.show()
                log_debug("显示OCR文字层", "OCR层")

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
        确保字符位置已计算（懒加载入口）
        
        只在首次鼠标交互时调用一次，避免加载时卡顿
        """
        if not self._char_positions_calculated and self.text_items:
            self.recalculate_char_positions()

    def _is_pos_on_text(self, pos: QPoint) -> bool:
        """给定本地坐标，判断是否在文字块扩展范围内"""
        if not self.text_items:
            return False
        
        scale_x, scale_y = self.get_scale_factors()
        for idx, item in enumerate(self.text_items):
            if item.contains(pos, scale_x, scale_y, self.original_width, self.original_height):
                # log_debug(f"鼠标在文字块 {idx} 上: {item.text[:10]}...", "OCR层")
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
        # 保存真实原始尺寸，用于 set_image_transform 映射
        self._true_orig_width = original_width
        self._true_orig_height = original_height
        self._text_union_rect = None
        
        # 重置懒加载标志
        self._char_positions_calculated = False

        items, union_rect = self.prepare_ocr_items(ocr_result)
        if not items:
            return

        self.text_items = items
        self._text_union_rect = union_rect

        # OCR 模块已按阅读顺序排序，无需再次排序
        
        # 优化：不立即计算字符位置，延迟到真正需要时（鼠标交互时）
        # 这样可以避免加载时 UI 卡顿，让用户可以立即移动窗口
        # self.recalculate_char_positions()  # 注释掉立即计算
        
        # 加载完成后，如果已启用则显示文字层
        if self.enabled:
            self._apply_effective_enabled()

    @staticmethod
    def prepare_ocr_items(ocr_result: Dict) -> Tuple[List["OCRTextItem"], Optional[QRectF]]:
        """预处理 OCR 结果为文字块列表与整体包围盒（可在子线程执行）。"""
        if not ocr_result or ocr_result.get('code') != 100:
            return [], None

        data = ocr_result.get('data', [])
        if not data:
            return [], None

        items: List[OCRTextItem] = []
        for item in data:
            text = item.get('text', '')
            box = item.get('box', [])
            score = item.get('score', 0.0)

            # 明确检查 text 和 box 是否有效（避免 numpy 数组的真值判断问题）
            if text and box is not None and len(box) > 0:
                items.append(OCRTextItem(text, box, score))

        if not items:
            return [], None

        min_x = min(item.norm_rect.left() for item in items)
        min_y = min(item.norm_rect.top() for item in items)
        max_x = max(item.norm_rect.right() for item in items)
        max_y = max(item.norm_rect.bottom() for item in items)
        union_rect = QRectF(min_x, min_y, max_x - min_x, max_y - min_y)
        return items, union_rect

    def load_prepared_ocr_items(
        self,
        items: List["OCRTextItem"],
        union_rect: Optional[QRectF],
        original_width: int,
        original_height: int,
    ):
        """加载预处理后的 OCR 文字块（主线程 UI 更新）。"""
        self.text_items = items
        self.original_width = original_width
        self.original_height = original_height
        # 保存真实原始尺寸，用于 set_image_transform 映射
        self._true_orig_width = original_width
        self._true_orig_height = original_height
        self._text_union_rect = union_rect
        self._char_positions_calculated = False

        if self.enabled:
            self._apply_effective_enabled()
    
    def get_scale_factors(self) -> tuple:
        """获取当前缩放比例"""
        if self.original_width == 0 or self.original_height == 0:
            return (1.0, 1.0)
        
        scale_x = self.width() / self.original_width
        scale_y = self.height() / self.original_height
        return (scale_x, scale_y)

    def set_image_transform(self, transform):
        """
        应用图像变换到 OCR 文字层

        将每个 OCRTextItem 的 norm_rect 从原始坐标映射到变换后坐标，
        并更新 original_width/height 为变换后尺寸。
        这样所有现有的 get_scaled_rect / get_scale_factors 代码无需修改。

        Args:
            transform: PinImageTransform 实例
        """
        if not self.text_items:
            return

        # 获取原始图像的真实尺寸（从第一个 item 的 _orig_norm_rect 所在空间推算）
        # 我们存储的 _orig_width/_orig_height 在 load 时已保存
        orig_w = getattr(self, '_true_orig_width', self.original_width)
        orig_h = getattr(self, '_true_orig_height', self.original_height)

        if not transform.has_transform:
            # 重置到原始状态
            for item in self.text_items:
                item.norm_rect = QRectF(item._orig_norm_rect)
            self.original_width = orig_w
            self.original_height = orig_h
        else:
            # 映射每个 item 的 norm_rect
            for item in self.text_items:
                item.norm_rect = transform.map_ocr_rect(
                    item._orig_norm_rect, orig_w, orig_h)
            # 更新参考尺寸为变换后尺寸
            mapped_w, mapped_h = transform.mapped_image_size(orig_w, orig_h)
            self.original_width = int(mapped_w)
            self.original_height = int(mapped_h)

        # 重新计算 union rect
        if self.text_items:
            min_x = min(item.norm_rect.left() for item in self.text_items)
            min_y = min(item.norm_rect.top() for item in self.text_items)
            max_x = max(item.norm_rect.right() for item in self.text_items)
            max_y = max(item.norm_rect.bottom() for item in self.text_items)
            self._text_union_rect = QRectF(min_x, min_y, max_x - min_x, max_y - min_y)

        # 触发字符位置重新计算
        self._char_positions_calculated = False
        self.clear_selection()
        self.update()
    
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
        line_tolerance = avg_height * 0.8
        
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
        
        selected_items = []
        
        for idx in range(start_item_idx, end_item_idx + 1):
            if idx >= len(self.text_items):
                break
            
            item = self.text_items[idx]
            
            if idx == start_item_idx and idx == end_item_idx:
                # 同一个文字块内选择
                text = item.text[start_char_idx:end_char_idx]
            elif idx == start_item_idx:
                # 起始文字块
                text = item.text[start_char_idx:]
            elif idx == end_item_idx:
                # 结束文字块
                text = item.text[:end_char_idx]
            else:
                # 中间的完整文字块
                text = item.text
            
            if text:
                center_y = item.norm_rect.y() + item.norm_rect.height() / 2
                height = item.norm_rect.height()
                selected_items.append({
                    "text": text,
                    "center_y": center_y,
                    "height": height,
                })
        
        if not selected_items:
            return ""
        
        if len(selected_items) == 1:
            return selected_items[0]["text"]
        
        avg_height = sum(b["height"] for b in selected_items) / len(selected_items)
        line_tolerance = avg_height * 0.5
        
        lines = []
        current_line = []
        current_line_y = None
        
        for block in selected_items:
            if current_line_y is None:
                current_line = [block["text"]]
                current_line_y = block["center_y"]
            elif abs(block["center_y"] - current_line_y) <= line_tolerance:
                current_line.append(block["text"])
            else:
                lines.append(" ".join(current_line))
                current_line = [block["text"]]
                current_line_y = block["center_y"]
        
        if current_line:
            lines.append(" ".join(current_line))
        
        return "\n".join(lines)
    
    @safe_event
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
            
            # 计算选择区域的起始和结束位置
            if idx == start_item_idx and idx == end_item_idx:
                p1 = item.char_positions[min(start_char_idx, len(item.char_positions) - 1)]
                p2 = item.char_positions[min(end_char_idx, len(item.char_positions) - 1)]
            elif idx == start_item_idx:
                p1 = item.char_positions[min(start_char_idx, len(item.char_positions) - 1)]
                p2 = (rect.y() + rect.height()) if item._is_vertical else (rect.x() + rect.width())
            elif idx == end_item_idx:
                p1 = rect.y() if item._is_vertical else rect.x()
                p2 = item.char_positions[min(end_char_idx, len(item.char_positions) - 1)]
            else:
                p1 = rect.y() if item._is_vertical else rect.x()
                p2 = (rect.y() + rect.height()) if item._is_vertical else (rect.x() + rect.width())
            
            # 绘制选择矩形（垂直文字沿 y 轴，水平文字沿 x 轴）
            if item._is_vertical:
                selection_rect = QRect(rect.x(), p1, rect.width(), p2 - p1)
            else:
                selection_rect = QRect(p1, rect.y(), p2 - p1, rect.height())
            painter.fillRect(selection_rect, selection_color)
    
    @safe_event
    def mousePressEvent(self, event):
        """鼠标按下事件 - Word 风格点击设置光标"""
        if self._is_parent_dragging():
            event.ignore()
            return
        
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
    
    @safe_event
    def mouseMoveEvent(self, event):
        """鼠标移动事件 - 动态切换光标 + 拖拽选择文字（懒加载优化）"""
        if self._is_parent_dragging():
            if self.is_selecting:
                self.is_selecting = False
            self.setCursor(Qt.CursorShape.ArrowCursor)
            event.ignore()
            return
        if not self._is_active():
            # 不活跃时停止选择并透传
            if self.is_selecting:
                self.is_selecting = False
            self.setCursor(Qt.CursorShape.ArrowCursor)
            event.ignore()
            return
        
        # 懒加载：首次鼠标移动时才计算字符位置
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
    
    @safe_event
    def mouseReleaseEvent(self, event):
        """鼠标释放事件"""
        if self._is_parent_dragging():
            event.ignore()
            return
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

        # 粗略过滤：点击点不在 OCR 文本整体包围盒内时，直接返回
        if strict and self._text_union_rect is not None:
            union_rect = QRect(
                int(self._text_union_rect.x() * scale_x),
                int(self._text_union_rect.y() * scale_y),
                int(self._text_union_rect.width() * scale_x),
                int(self._text_union_rect.height() * scale_y),
            )
            if not union_rect.contains(pos):
                return (None, None)

        nearest_idx = None
        nearest_dist = None
        nearest_rect = None

        for item_idx, item in enumerate(self.text_items):
            rect = item.get_scaled_rect(scale_x, scale_y, self.original_width, self.original_height)

            # 使用扩展的检测范围
            expanded_rect = rect.adjusted(-5, -5, 5, 5)

            # 计算最近的文字块：垂直文字用水平距离，水平文字用垂直距离
            is_vert = rect.height() > rect.width() * 1.5
            dist = abs(pos.x() - rect.center().x()) if is_vert else abs(pos.y() - rect.center().y())
            if nearest_dist is None or dist < nearest_dist:
                nearest_dist = dist
                nearest_idx = item_idx
                nearest_rect = rect

            if expanded_rect.contains(pos):
                if not item.char_positions:
                    item.calculate_char_positions(rect)
                char_idx = item.get_char_index_at_pos(pos.x(), rect, pos.y())
                return (item_idx, char_idx)

        # 如果是严格模式，未命中则返回 None
        if strict:
            return (None, None)

        # 未命中任何块时，选择最近行
        if nearest_idx is not None and nearest_rect is not None:
            item = self.text_items[nearest_idx]
            if not item.char_positions:
                item.calculate_char_positions(nearest_rect)

            # 超出时也要选择：左侧/上方=开头，右侧/下方=末尾
            char_idx = item.get_char_index_at_pos(pos.x(), nearest_rect, pos.y())
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
        
        # 直接使用 get_selected_text() 方法，它包含完整的分行逻辑
        selected_text = self.get_selected_text()
        
        if selected_text:
            # 复制到剪贴板
            clipboard = QApplication.clipboard()
            clipboard.setText(selected_text)
            log_info(f"已复制: {selected_text[:50]}{'...' if len(selected_text) > 50 else ''}", module="OCR文字层")
    
    @safe_event
    def keyPressEvent(self, event):
        """键盘事件"""
        if not self._is_active():
            # [WARN] 关键：禁用时不处理事件，但要透传给父窗口
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
    
    @safe_event
    def resizeEvent(self, event):
        """窗口缩放时重新计算字符位置（优化：仅标记需要重新计算）"""
        super().resizeEvent(event)
        # 优化：只标记需要重新计算，下次鼠标交互时才真正计算
        self._char_positions_calculated = False
        # 如果用户正在选择文字，立即重新计算，保证选择准确
        if self.is_selecting or self.selection_start:
            self.recalculate_char_positions()
    
    def cleanup(self):
        """清理资源"""
        try:
            # 清除文字块
            if hasattr(self, 'text_items'):
                self.text_items.clear()
            
            # 重置懒加载标志
            self._char_positions_calculated = False
            self.clear_selection()
        except Exception as e:
            # 静默处理清理错误，避免影响父窗口的关闭流程
            log_debug(f"OCR文字层清理时出错: {e}", "OCR")
    
    @safe_event
    def closeEvent(self, event):
        """窗口关闭时清理"""
        try:
            self.cleanup()
        except Exception as e:
            # 即使cleanup失败也要继续关闭流程
            log_debug(f"OCR文字层closeEvent时出错: {e}", "OCR")
        finally:
            # 确保调用父类的closeEvent
            try:
                super().closeEvent(event)
            except Exception as e:
                log_exception(e, "OCR文字层super closeEvent")
 