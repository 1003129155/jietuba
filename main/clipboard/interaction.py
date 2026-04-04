# -*- coding: utf-8 -*-
"""
剪贴板窗口交互处理模块

统一管理键盘和鼠标的交互逻辑：
- 键盘上下键选择
- 鼠标点击/悬停选择
- 选中项高亮显示
- 预览弹窗管理
"""

from typing import Optional, Callable
from PySide6.QtCore import QObject, Qt, QEvent, Signal
from PySide6.QtWidgets import QListWidget, QListWidgetItem
from PySide6.QtGui import QKeyEvent, QCursor

from .preview_popup import PreviewPopup
from .data_manager import ClipboardItem
from core.logger import log_debug
from core import safe_event


class SelectionManager(QObject):
    """
    选择管理器
    
    统一管理键盘和鼠标的选择逻辑，实现：
    1. 键盘上下键导航
    2. 鼠标点击选择
    3. 选中项高亮和预览
    """
    
    # 信号
    selection_changed = Signal(object)  # 选中项变化，参数为 item_id 或 None
    item_activated = Signal(int)  # 项目被激活（Enter或双击），参数为 item_id
    request_sidebar_focus = Signal()  # 请求切换到侧边分组栏（left/right 模式）
    request_group_switch = Signal(int)  # 顶部模式左右键切换分组，delta=-1/+1
    
    def __init__(self, list_widget: QListWidget, get_item_data: Callable[[int], Optional[ClipboardItem]]):
        """
        初始化选择管理器
        
        Args:
            list_widget: 列表控件
            get_item_data: 获取列表项对应的 ClipboardItem 数据的函数
        """
        super().__init__()
        self.list_widget = list_widget
        self.get_item_data = get_item_data
        
        # 分组栏位置，由外部在布局变更时同步设置
        self.group_bar_position: str = "right"  # "top" / "left" / "right"
        
        # 当前选中的项索引（-1 表示没有选中）
        self._selected_index: int = -1
        
        # 鼠标悬停的项索引（-1 表示没有悬停）
        self._hovered_index: int = -1
        
        # 是否已经开始键盘导航（呼出后首次按方向键才开始）
        self._keyboard_navigation_started: bool = False
        
        # 安装事件过滤器来捕获键盘事件
        self.list_widget.installEventFilter(self)

        # 搜索框（可在构建后通过 set_search_input 注册）
        self._search_input = None
        
        # 连接列表的点击信号
        self.list_widget.itemClicked.connect(self._on_item_clicked)
        
        # 监听列表的当前项变化（用于同步 QListWidget 内部状态）
        self.list_widget.currentItemChanged.connect(self._on_current_item_changed)
    
    def reset(self):
        """重置选择状态（窗口打开时调用）"""
        self._selected_index = -1
        self._hovered_index = -1
        self._keyboard_navigation_started = False
        # 清除列表的选中状态
        self.list_widget.clearSelection()
        self.list_widget.setCurrentItem(None)
        # 隐藏预览
        PreviewPopup.instance().hide_preview()
        log_debug("🔄 SelectionManager 已重置", "Clipboard")

    def clear_selection(self, reset_keyboard_state: bool = False):
        """清除当前选中（不一定重置键盘导航状态）"""
        self._selected_index = -1
        self._hovered_index = -1
        if reset_keyboard_state:
            self._keyboard_navigation_started = False
        self.list_widget.clearSelection()
        self.list_widget.setCurrentItem(None)
        PreviewPopup.instance().hide_preview()

    def select_item_id(self, item_id: int) -> bool:
        """根据 item_id 选中项（返回是否成功）"""
        if item_id is None:
            return False
        for i in range(self.list_widget.count()):
            item = self.list_widget.item(i)
            if item and item.data(Qt.ItemDataRole.UserRole) == item_id:
                self._keyboard_navigation_started = True
                self._select_index(i)
                return True
        return False
    
    def set_search_input(self, search_input):
        """注册搜索框，使其方向键可导航列表"""
        self._search_input = search_input
        search_input.installEventFilter(self)

    @safe_event
    def eventFilter(self, obj, event: QEvent) -> bool:
        """事件过滤器，捕获键盘事件"""

        # ── 搜索框：方向键转发给列表导航，其余键放行 ──────────────────
        if self._search_input is not None and obj is self._search_input \
                and event.type() == QEvent.Type.KeyPress:
            key_event: QKeyEvent = event
            key = key_event.key()
            pos = self.group_bar_position

            if key == Qt.Key.Key_Up:
                self._move_selection(-1)
                self.list_widget.setFocus()
                return True
            elif key == Qt.Key.Key_Down:
                self._move_selection(1)
                self.list_widget.setFocus()
                return True
            elif key in (Qt.Key.Key_Return, Qt.Key.Key_Enter):
                self._activate_current()
                return True
            elif key == Qt.Key.Key_Right and pos == "top":
                self.request_group_switch.emit(1)
                return True
            elif key == Qt.Key.Key_Left and pos == "top":
                self.request_group_switch.emit(-1)
                return True
            # 其余键（文字输入、退格等）正常放行，不消费
            return False

        if obj == self.list_widget and event.type() == QEvent.Type.KeyPress:
            key_event: QKeyEvent = event
            key = key_event.key()
            pos = self.group_bar_position

            if key == Qt.Key.Key_Up:
                self._move_selection(-1)
                return True
            elif key == Qt.Key.Key_Down:
                self._move_selection(1)
                return True
            elif key in (Qt.Key.Key_Return, Qt.Key.Key_Enter):
                self._activate_current()
                return True
            elif key == Qt.Key.Key_Right:
                if pos == "top":
                    # 顶部模式：右键直接切换到下一个分组
                    self.request_group_switch.emit(1)
                    return True
                elif pos == "right":
                    self.request_sidebar_focus.emit()
                    return True
            elif key == Qt.Key.Key_Left:
                if pos == "top":
                    # 顶部模式：左键直接切换到上一个分组
                    self.request_group_switch.emit(-1)
                    return True
                elif pos == "left":
                    self.request_sidebar_focus.emit()
                    return True
            elif key == Qt.Key.Key_Escape:
                # 如果有选中项，先取消选中；否则关闭窗口
                if self._selected_index >= 0:
                    self.reset()
                    return True
                # 返回 False 让窗口处理关闭
                return False
        
        return super().eventFilter(obj, event)
    
    def _move_selection(self, delta: int):
        """
        移动选择
        
        Args:
            delta: 移动方向，-1 向上，+1 向下
        """
        count = self.list_widget.count()
        if count == 0:
            return
        
        # 首次按方向键，从当前项或悬停项开始
        if not self._keyboard_navigation_started:
            self._keyboard_navigation_started = True
            
            # 优先使用悬停位置
            if self._hovered_index >= 0:
                new_index = self._hovered_index + delta
            else:
                current_row = self.list_widget.currentRow()
                if current_row >= 0 and self.list_widget.selectedItems():
                    # 如果已经有选中项，从这里继续
                    new_index = current_row + delta
                elif delta > 0:
                    # 按下键，选中第一项
                    new_index = 0
                else:
                    # 按上键，选中最后一项
                    new_index = count - 1
        else:
            # 已经开始导航，正常移动
            new_index = self._selected_index + delta
            
            # 边界处理：循环或限制
            if new_index < 0:
                new_index = 0  # 限制在顶部（不循环）
            elif new_index >= count:
                new_index = count - 1  # 限制在底部（不循环）

        # 统一边界保护（首次也需要）
        if new_index < 0:
            new_index = 0
        elif new_index >= count:
            new_index = count - 1
        
        # 更新选择
        self._select_index(new_index)
    
    def _select_index(self, index: int):
        """
        选中指定索引的项
        
        Args:
            index: 要选中的索引
        """
        if index < 0 or index >= self.list_widget.count():
            return
        
        # 更新内部状态
        old_index = self._selected_index
        self._selected_index = index
        
        # 更新列表控件的选中状态
        item = self.list_widget.item(index)
        if item:
            self.list_widget.setCurrentItem(item)
            self.list_widget.scrollToItem(item)
            
            # 显示预览
            self._show_preview_for_item(item)
            
            # 发出信号
            item_id = item.data(Qt.ItemDataRole.UserRole)
            self.selection_changed.emit(item_id)
            
            
    
    def _on_item_clicked(self, item: QListWidgetItem):
        """鼠标点击列表项"""
        if not item:
            return
        
        item_id = item.data(Qt.ItemDataRole.UserRole)
        if not item_id:
            return
        
        # 直接触发激活（粘贴）
        self.item_activated.emit(item_id)
    
    def _on_current_item_changed(self, current: QListWidgetItem, previous: QListWidgetItem):
        """列表当前项变化（同步内部状态）"""
        if current:
            index = self.list_widget.row(current)
            if index != self._selected_index:
                self._selected_index = index
    
    def _show_preview_for_item(self, item: QListWidgetItem):
        """为指定项显示预览"""
        item_id = item.data(Qt.ItemDataRole.UserRole)
        if not item_id:
            return
        
        # 获取项的数据
        clip_item = self.get_item_data(item_id)
        if not clip_item:
            return
        
        # 计算预览位置（优先左侧，避免遮挡主窗口）
        item_rect = self.list_widget.visualItemRect(item)
        if not item_rect.isNull():
            global_pos = self.list_widget.mapToGlobal(item_rect.topLeft())
            window_rect = self.list_widget.window().frameGeometry() if self.list_widget.window() else None
            
            # 显示预览
            popup = PreviewPopup.instance()
            popup.show_preview(clip_item, global_pos, delay_ms=0, prefer_side="left", avoid_rect=window_rect)  # 键盘选择时立即显示
    
    def _activate_current(self):
        """激活当前选中项（Enter键）"""
        if self._selected_index < 0:
            return
        
        item = self.list_widget.item(self._selected_index)
        if item:
            item_id = item.data(Qt.ItemDataRole.UserRole)
            if item_id:
                log_debug(f"✅ 激活项: {item_id}", "Clipboard")
                self.item_activated.emit(item_id)
    
    def set_hovered_index(self, index: int):
        """设置悬停索引(供外部调用)"""
        self._hovered_index = index
    
    def clear_hovered_index(self):
        """清除悬停索引(供外部调用)"""
        self._hovered_index = -1
    
    def get_current_item_id(self) -> Optional[int]:
        """获取当前选中项的 ID"""
        if self._selected_index < 0:
            return None
        
        item = self.list_widget.item(self._selected_index)
        if item:
            return item.data(Qt.ItemDataRole.UserRole)
        return None
    
    def has_selection(self) -> bool:
        """是否有选中项"""
        return self._selected_index >= 0
    
    def select_first(self):
        """选中第一项"""
        if self.list_widget.count() > 0:
            self._keyboard_navigation_started = True
            self._select_index(0)
    
    def select_last(self):
        """选中最后一项"""
        count = self.list_widget.count()
        if count > 0:
            self._keyboard_navigation_started = True
            self._select_index(count - 1)
 