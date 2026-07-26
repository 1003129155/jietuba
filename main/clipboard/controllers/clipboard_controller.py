# -*- coding: utf-8 -*-
"""
剪贴板控制器 - 业务逻辑层

负责数据加载、搜索筛选、分组管理、项目操作、侧边栏溢出计算等业务逻辑。
"""

import ctypes
import json
import os
import re
from datetime import datetime, time, timedelta
from typing import Optional, List, Callable, Tuple
from time import perf_counter
from PySide6.QtCore import QObject, Signal, QTimer, Qt
from ui.dialogs import show_confirm_dialog

from ..core import ClipboardManager, ClipboardItem, Group, GroupType
from ..ui.dialogs.manage_dialog import get_manage_dialog, get_existing_manage_dialog
from .context_menu_controller import ClipboardContextMenuController, ContextMenuData, MenuAction
from core.logger import log_debug, log_info, log_error, log_exception


# ============================================================
# 侧边栏溢出计算
# ============================================================

# 布局常量（与 window.py 右栏 UI 一致）
_TOP_USED = 8 + 34 + 4 + 1 + 4 + 34 + 4
_BOTTOM_RESERVED = 8 + 34 + 4 + 34 + 8
_BTN_SLOT = 34 + 4

# 水平布局常量（top 模式：按钮横排）
_H_LEFT_USED = 2 + 34 + 4 + 1 + 4 + 34 + 4
_H_RIGHT_RESERVED = 4 + 34 + 4 + 34 + 2
_H_BTN_SLOT = 34 + 4


def calc_sidebar_capacity(right_bar_height: int) -> int:
    """计算右侧按钮栏在当前高度下最多能显示的分组按钮数量。-1 表示未初始化。"""
    if right_bar_height <= 0:
        return -1
    available = right_bar_height - _TOP_USED - _BOTTOM_RESERVED
    return 0 if available <= 0 else available // _BTN_SLOT


def calc_topbar_capacity(bar_width: int) -> int:
    """计算顶部横栏在当前宽度下最多能显示的分组按钮数量。"""
    if bar_width <= 0:
        return -1
    available = bar_width - _H_LEFT_USED - _H_RIGHT_RESERVED
    return 0 if available <= 0 else available // _H_BTN_SLOT


# Windows API 常量
VK_CONTROL = 0x11
VK_V = 0x56
KEYEVENTF_KEYUP = 0x0002


def get_foreground_window():
    """获取当前前台窗口句柄"""
    try:
        return ctypes.windll.user32.GetForegroundWindow()
    except Exception as e:
        log_exception(e, "获取前台窗口")
        return None


def set_foreground_window(hwnd):
    """设置前台窗口"""
    try:
        if hwnd:
            ctypes.windll.user32.SetForegroundWindow(hwnd)
            return True
    except Exception as e:
        log_exception(e, "设置前台窗口")
    return False


def send_ctrl_v():
    """
    发送 Ctrl+V 按键事件
    
    使用 Windows API 模拟按键，实现自动粘贴。
    """
    try:
        # 按下 Ctrl
        ctypes.windll.user32.keybd_event(VK_CONTROL, 0, 0, 0)
        # 按下 V
        ctypes.windll.user32.keybd_event(VK_V, 0, 0, 0)
        # 释放 V
        ctypes.windll.user32.keybd_event(VK_V, 0, KEYEVENTF_KEYUP, 0)
        # 释放 Ctrl
        ctypes.windll.user32.keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0)
        return True
    except Exception as e:
        log_error(f"发送 Ctrl+V 失败: {e}", "Clipboard")
        return False


class ClipboardController(QObject):
    """
    剪贴板窗口控制器
    
    负责处理剪贴板窗口的业务逻辑，包括：
    - 数据加载与分页
    - 搜索与筛选
    - 分组管理
    - 项目操作（粘贴、删除、置顶等）
    - 设置管理
    - 为独立的右键菜单控制器提供状态与查询入口
    """
    
    # 信号定义
    data_loaded = Signal(list, bool)  # (items, is_first_page) 数据加载完成
    loading_state_changed = Signal(bool)  # 加载状态变化
    reload_required = Signal()  # 需要重新加载
    load_completed = Signal()  # 单次加载完全完成（_is_loading 已设为 False）
    
    def __init__(self, manager: ClipboardManager):
        super().__init__()
        self.manager = manager
        
        # 当前数据状态
        self.current_items: List[ClipboardItem] = []
        self.current_group_id: Optional[int] = None  # None 表示显示剪切板历史
        
        # 分页加载相关
        self._current_offset = 0
        self._page_size = 38
        self._page_size_without_metadata = 65  # 关闭时间来源显示时的每页数量（更多）
        self._is_loading = False
        self._has_more = True
        self._last_scroll_value = 0
        self._pending_reload = False
        
        # 搜索和筛选状态
        self._search_text: Optional[str] = None
        self._content_type: Optional[str] = None  # None, "text", "image", "file"
        self._time_range: Optional[Tuple[datetime, datetime]] = None
        
        # 设置
        self.auto_paste_enabled = True
        self.paste_with_html = True
        
        # 记录打开窗口前的活动窗口
        self._previous_window_hwnd = None

        # 右键菜单数据控制逻辑已抽到独立模块
        self._context_menu_controller = ClipboardContextMenuController(self)
        
        # 加载设置
        self._load_settings()
    
    # ==================== 设置管理 ====================
    
    def _load_settings(self):
        """加载设置"""
        try:
            from settings import get_tool_settings_manager
            config = get_tool_settings_manager()
            self.auto_paste_enabled = config.get_clipboard_auto_paste()
            self.paste_with_html = config.get_app_setting("clipboard_paste_with_html", True)
        except Exception as e:
            log_exception(e, "加载剪贴板设置")
            # 默认开启自动粘贴和带格式粘贴
            self.auto_paste_enabled = True
            self.paste_with_html = True
    
    def set_auto_paste(self, enabled: bool):
        """设置自动粘贴"""
        # 立即更新属性，使设置立即生效
        self.auto_paste_enabled = enabled
        try:
            from settings import get_tool_settings_manager
            config = get_tool_settings_manager()
            config.set_clipboard_auto_paste(enabled)
        except Exception as e:
            log_exception(e, "保存自动粘贴设置")
    
    def set_paste_with_html(self, enabled: bool):
        """设置带格式粘贴"""
        self.paste_with_html = enabled
        try:
            from settings import get_tool_settings_manager
            config = get_tool_settings_manager()
            config.set_app_setting("clipboard_paste_with_html", enabled)
        except Exception as e:
            log_exception(e, "保存带格式粘贴设置")
    
    def set_move_to_top_on_paste(self, enabled: bool):
        """设置粘贴后移到最前"""
        try:
            from settings import get_tool_settings_manager
            config = get_tool_settings_manager()
            config.set_clipboard_move_to_top_on_paste(enabled)
        except Exception as e:
            log_exception(e, "保存粘贴后移到最前设置")
    
    # ==================== 数据加载 ====================
    
    def _get_page_size(self) -> int:
        """获取当前应该使用的每页数量（根据是否显示时间来源）"""
        try:
            from settings import get_tool_settings_manager
            config = get_tool_settings_manager()
            show_metadata = config.get_clipboard_show_metadata()
            # 如果显示时间来源，用较小的值；如果不显示，用较大的值
            return self._page_size if show_metadata else self._page_size_without_metadata
        except Exception as e:
            log_exception(e, "获取剪贴板每页数量")
            return self._page_size
    
    def load_history(self):
        """加载历史记录（重置并加载第一页）"""
        if self._is_loading:
            self._pending_reload = True
            log_debug("⏸️ 正在加载中，标记待重新加载", "Clipboard")
            return
        
        # 重置分页状态
        self._current_offset = 0
        self._has_more = True
        self.current_items = []
        self._pending_reload = False
        self._last_scroll_value = 0
        
        # 加载第一页
        self._load_more_items()
    
    def has_more_items(self) -> bool:
        """检查是否还有更多数据可以加载"""
        result = self._has_more and not self._is_loading
        return result
    
    def _load_more_items(self):
        """加载更多项目（分页加载）"""
        if self._is_loading or not self._has_more:
            return
        
        self._is_loading = True
        self.loading_state_changed.emit(True)
        
        # 动态获取 page_size（根据是否显示时间来源）
        current_page_size = self._get_page_size()
        
        t_total_start = perf_counter()
        
        try:
            
            # 根据当前分组加载内容
            t_query_start = perf_counter()
            is_first_page = (self._current_offset == 0)
            new_items, raw_count = self._fetch_items_page(current_page_size, self._current_offset)
            self._current_offset += raw_count
            
            t_query_end = perf_counter()
            log_info(f"加载完成 - 获取到 {len(new_items)} 条记录", "Clipboard")
            
            # 检查是否还有更多数据（使用动态的 page_size）
            if len(new_items) < current_page_size:
                self._has_more = False
            
            # 追加到当前列表
            if new_items:
                self.current_items.extend(new_items)
            
            # 发送数据加载完成信号
            self.data_loaded.emit(new_items, is_first_page)
            
        except Exception as e:
            log_error(f"加载数据失败: {e}", "Clipboard")
            import traceback
            traceback.print_exc()
        
        finally:
            t_total_end = perf_counter()
            self._is_loading = False
            self.loading_state_changed.emit(False)
            
            # 发出加载完成信号（此时 _is_loading 已经是 False）
            self.load_completed.emit()
            
            # 检查是否有待处理的重新加载请求
            if self._pending_reload:
                self._pending_reload = False
                QTimer.singleShot(0, self.load_history)
    
    def load_more_if_needed(self):
        """根据需要加载更多数据"""
        if not self._has_more or self._is_loading:
            return
        self._load_more_items()

    def _fetch_items_page(self, limit: int, offset: int) -> Tuple[List[ClipboardItem], int]:
        if self.current_group_id is None:
            start_ts = int(self._time_range[0].timestamp()) if self._time_range else None
            end_ts = int(self._time_range[1].timestamp()) if self._time_range else None
            items = self.manager.get_history(
                limit=limit,
                offset=offset,
                search=self._search_text,
                content_type=self._content_type,
                start_time=start_ts,
                end_time=end_ts,
            )
            return items, len(items)

        items = self.manager.get_by_group(
            group_id=self.current_group_id,
            limit=limit,
            offset=offset
        )
        raw_count = len(items)

        if self._search_text:
            search_lower = self._search_text.lower()
            items = [
                item for item in items
                if search_lower in item.content.lower()
                or (item.title and search_lower in item.title.lower())
            ]

        return items, raw_count
    
    def check_scroll_load(self, scroll_value: int, scroll_max: int):
        """检查滚动位置，决定是否加载更多
        
        Args:
            scroll_value: 当前滚动位置
            scroll_max: 滚动条最大值
        """
        if not self._has_more or self._is_loading:
            return
        
        # 如果没有滚动条（maximum <= 0），不触发加载
        if scroll_max <= 0:
            return
        
        # 只在向下滚动时触发加载
        if scroll_value <= self._last_scroll_value:
            self._last_scroll_value = scroll_value
            return
        
        self._last_scroll_value = scroll_value
        
        # 计算距离底部的距离
        distance_to_bottom = scroll_max - scroll_value
        
        # 计算当前滚动位置的百分比
        scroll_percentage = (scroll_value / scroll_max * 100) if scroll_max > 0 else 0
        
        # 必须满足两个条件才触发加载：
        # 1. 距离底部小于 50 像素
        # 2. 滚动位置超过 90%
        if distance_to_bottom < 50 and scroll_percentage > 90:
            log_debug(f"🔄 触发加载更多 - 距离底部: {distance_to_bottom}px, 滚动位置: {scroll_percentage:.1f}%", "Clipboard")
            self._load_more_items()
    
    # ==================== 搜索与筛选 ====================
    
    def set_search_text(self, text: str):
        """设置搜索文本"""
        search = text.strip() or None
        if self._search_text != search:
            self._search_text = search
            self.load_history()
    
    def set_content_type_filter(self, filter_index: int):
        """设置内容类型筛选
        
        Args:
            filter_index: 0=全部, 1=文本, 2=图片, 3=文件
        """
        type_map = {0: None, 1: "text", 2: "image", 3: "file"}
        content_type = type_map.get(filter_index)
        if self._content_type != content_type:
            self._content_type = content_type
            self.load_history()

    def set_time_range_text(self, range_text: str) -> bool:
        """设置时间区间筛选，格式示例：2026/06/20-2026/06/30。"""
        parsed_range = self._parse_time_range_text(range_text)
        if parsed_range is None and range_text.strip():
            return False
        if self._time_range != parsed_range:
            self._time_range = parsed_range
            self.load_history()
        return True

    def clear_time_and_type_filters(self):
        """清空时间区间和内容类型筛选。"""
        if self._time_range is not None or self._content_type is not None:
            self._time_range = None
            self._content_type = None
            self.load_history()

    @staticmethod
    def _parse_time_range_text(range_text: str) -> Optional[Tuple[datetime, datetime]]:
        text = range_text.strip()
        if not text:
            return None

        match = re.match(
            r"^\s*(\d{4})[\/.-](\d{1,2})[\/.-](\d{1,2})\s*(?:-|~|至|到)\s*"
            r"(\d{4})[\/.-](\d{1,2})[\/.-](\d{1,2})\s*$",
            text,
        )
        if not match:
            return None

        try:
            start_date = datetime(
                int(match.group(1)),
                int(match.group(2)),
                int(match.group(3)),
            ).date()
            end_date = datetime(
                int(match.group(4)),
                int(match.group(5)),
                int(match.group(6)),
            ).date()
        except ValueError:
            return None

        if end_date < start_date:
            return None

        start = datetime.combine(start_date, time.min)
        end = datetime.combine(end_date + timedelta(days=1), time.min)
        return start, end
    
    # ==================== 分组管理 ====================
    
    def switch_to_group(self, group_id: Optional[int]):
        """切换到指定分组
        
        Args:
            group_id: 分组ID，None 表示切换到剪切板历史
        """
        if self.current_group_id != group_id:
            self.current_group_id = group_id
            self.load_history()
    
    def delete_group(self, group_id: int, parent_widget=None) -> bool:
        """删除分组
        
        Args:
            group_id: 分组ID
            parent_widget: 父窗口（用于显示确认对话框）
        
        Returns:
            是否删除成功
        """
        # 显示确认对话框
        if parent_widget:
            title = parent_widget.tr("Confirm Delete") if hasattr(parent_widget, "tr") else "Confirm Delete"
            if hasattr(parent_widget, "tr"):
                message = "\n".join([
                    parent_widget.tr("Are you sure you want to delete this group?"),
                    parent_widget.tr("All items in the group will also be deleted."),
                ])
            else:
                message = "Are you sure you want to delete this group?\nAll items in the group will also be deleted."
            reply = show_confirm_dialog(
                parent_widget,
                title,
                message
            )
            if not reply:
                return False

        # 删除分组
        if self.manager.delete_group(group_id):
            # 如果当前正在显示被删除的分组，切换到剪切板
            if self.current_group_id == group_id:
                self.current_group_id = None
            dialog = get_existing_manage_dialog()
            if dialog is not None:
                dialog.refresh_after_external_change(deleted_group_id=group_id)
            self.reload_required.emit()
            return True
        return False
    
    def get_groups(self) -> List[Group]:
        """获取所有分组"""
        return self.manager.get_groups()

    def get_group_move_state(self, group_id: int) -> tuple[bool, bool]:
        """获取分组是否可上移/下移"""
        groups = self.manager.get_groups()
        for index, group in enumerate(groups):
            if group.id == group_id:
                return index > 0, index < len(groups) - 1
        return False, False

    def move_group_order(self, group_id: int, direction: int) -> bool:
        """移动分组顺序（direction: -1 上移, 1 下移）"""
        groups = self.manager.get_groups()
        current_index = next((i for i, g in enumerate(groups) if g.id == group_id), None)
        if current_index is None:
            return False

        new_index = current_index + (-1 if direction < 0 else 1)
        if new_index < 0 or new_index >= len(groups):
            return False

        temp_groups = [g for i, g in enumerate(groups) if i != current_index]
        adjusted_new_index = new_index

        before_id = temp_groups[adjusted_new_index - 1].id if adjusted_new_index > 0 else None
        after_id = temp_groups[adjusted_new_index].id if adjusted_new_index < len(temp_groups) else None

        return self.manager.move_group_between(group_id, before_id=before_id, after_id=after_id)
    
    # ==================== 项目操作 ====================
    
    def paste_item(self, item_id: int, on_close_callback: Optional[Callable] = None) -> bool:
        """粘贴项目
        
        Args:
            item_id: 项目ID
            on_close_callback: 关闭窗口的回调函数
        
        Returns:
            是否粘贴成功
        """
        # 读取"粘贴后移到最前"设置
        from settings import get_tool_settings_manager
        config = get_tool_settings_manager()
        move_to_top = config.get_clipboard_move_to_top_on_paste()
        
        # 关键：只在"剪贴板历史"视图时才移动到最前
        # 如果在"收藏分组"视图，则不移动顺序
        if self.current_group_id is not None:
            move_to_top = False  # 在分组中粘贴，不移动顺序
        
        if self.manager.paste_item(item_id, self.paste_with_html, move_to_top):
            log_info(f"已粘贴项 {item_id} (带格式: {self.paste_with_html}, 移到最前: {move_to_top})", "Clipboard")
            
            # 调用关闭回调
            if on_close_callback:
                on_close_callback()
            
            # 自动粘贴：发送 Ctrl+V
            if self.auto_paste_enabled:
                # 先恢复之前的窗口焦点，再发送 Ctrl+V
                def do_paste():
                    if self._previous_window_hwnd:
                        set_foreground_window(self._previous_window_hwnd)
                    # 稍微延迟确保焦点切换完成
                    QTimer.singleShot(30, send_ctrl_v)
                
                # 延迟执行，确保剪贴板窗口已关闭/隐藏
                QTimer.singleShot(50, do_paste)
            
            return True
        return False

    def paste_transformed_text(
        self, item_id: int, transform_key: str,
        on_close_callback: Optional[Callable] = None,
    ) -> bool:
        """对文本项内容做加工后写入系统剪贴板并触发粘贴。

        Args:
            item_id: 项目ID
            transform_key: 转换键名（对应 TRANSFORM_REGISTRY 的 key，
                特殊值 "special_paste_plain_text" 表示粘贴纯文本不带格式）
            on_close_callback: 关闭窗口的回调函数

        Returns:
            是否成功
        """
        import pyclipboard

        # "保持顺序粘贴"：带格式粘贴，但强制不移到最前
        if transform_key == "special_paste_in_order":
            if self.manager.paste_item(item_id, self.paste_with_html, move_to_top=False):
                log_info(f"保持顺序粘贴项 {item_id}", "Clipboard")
                if on_close_callback:
                    on_close_callback()
                if self.auto_paste_enabled:
                    def do_paste_order():
                        if self._previous_window_hwnd:
                            set_foreground_window(self._previous_window_hwnd)
                        QTimer.singleShot(30, send_ctrl_v)
                    QTimer.singleShot(50, do_paste_order)
                return True
            return False

        # "粘贴纯文本"：等价于不带 HTML 格式粘贴
        if transform_key == "special_paste_plain_text":
            clipboard_item = self.get_item(item_id)
            if clipboard_item is None or clipboard_item.content_type != "text":
                log_error(f"项 {item_id} 不是文本类型，无法粘贴纯文本", "Clipboard")
                return False
            try:
                pyclipboard.set_clipboard_text(clipboard_item.content)
            except Exception as e:
                log_exception(e, "设置剪贴板纯文本失败")
                return False
            log_info(f"粘贴纯文本项 {item_id}", "Clipboard")
            if on_close_callback:
                on_close_callback()
            if self.auto_paste_enabled:
                def do_paste():
                    if self._previous_window_hwnd:
                        set_foreground_window(self._previous_window_hwnd)
                    QTimer.singleShot(30, send_ctrl_v)
                QTimer.singleShot(50, do_paste)
            return True

        from ..core.text_transform import TRANSFORM_REGISTRY

        transform_fn = TRANSFORM_REGISTRY.get(transform_key)
        if transform_fn is None:
            log_error(f"未知转换键: {transform_key}", "Clipboard")
            return False

        # 获取原始文本
        clipboard_item = self.get_item(item_id)
        if clipboard_item is None or clipboard_item.content_type != "text":
            log_error(f"项 {item_id} 不是文本类型，无法特殊粘贴", "Clipboard")
            return False

        original_text = clipboard_item.content

        # 执行文本转换
        try:
            transformed = transform_fn(original_text)
        except Exception as e:
            log_exception(e, f"文本转换失败: {transform_key}")
            return False

        # 写入系统剪贴板（纯文本，不带格式）
        try:
            pyclipboard.set_clipboard_text(transformed)
        except Exception as e:
            log_exception(e, "设置剪贴板文本失败")
            return False

        log_info(f"特殊粘贴项 {item_id} ({transform_key})", "Clipboard")

        # 调用关闭回调
        if on_close_callback:
            on_close_callback()

        # 自动粘贴：发送 Ctrl+V
        if self.auto_paste_enabled:
            def do_paste():
                if self._previous_window_hwnd:
                    set_foreground_window(self._previous_window_hwnd)
                QTimer.singleShot(30, send_ctrl_v)

            QTimer.singleShot(50, do_paste)

        return True

    def paste_file_text(
        self, item_id: int, transform_key: str,
        on_close_callback: Optional[Callable] = None,
    ) -> bool:
        """将文件项转换为纯文本后写入剪贴板并触发粘贴。"""
        import pyclipboard

        clipboard_item = self.get_item(item_id)
        if clipboard_item is None or clipboard_item.content_type != "file":
            log_error(f"项 {item_id} 不是文件类型，无法特殊粘贴", "Clipboard")
            return False

        try:
            data = json.loads(clipboard_item.content)
            files = data.get("files", []) if isinstance(data, dict) else []
            files = [os.path.normpath(file_path) for file_path in files if file_path]
        except Exception as e:
            log_exception(e, "解析文件项内容失败")
            return False

        if not files:
            log_error(f"文件项 {item_id} 没有可粘贴的文件路径", "Clipboard")
            return False

        if transform_key == "file_paste_names":
            text = "\n".join(os.path.basename(file_path) for file_path in files)
        elif transform_key == "file_paste_links":
            text = "\n".join(files)
        else:
            log_error(f"未知文件粘贴键: {transform_key}", "Clipboard")
            return False

        try:
            pyclipboard.set_clipboard_text(text)
        except Exception as e:
            log_exception(e, "设置剪贴板文件文本失败")
            return False

        log_info(f"文件特殊粘贴项 {item_id} ({transform_key})", "Clipboard")

        if on_close_callback:
            on_close_callback()

        if self.auto_paste_enabled:
            def do_paste():
                if self._previous_window_hwnd:
                    set_foreground_window(self._previous_window_hwnd)
                QTimer.singleShot(30, send_ctrl_v)

            QTimer.singleShot(50, do_paste)

        return True

    def delete_item(self, item_id: int) -> bool:
        """删除项目"""
        if self.manager.delete_item(item_id):
            self.load_history()
            return True
        return False

    def toggle_pin(self, item_id: int):
        """切换置顶"""
        self.manager.toggle_pin(item_id)
        self.load_history()
    
    def move_to_group(self, item_id: int, group_id: Optional[int]) -> bool:
        """将项目移动到分组"""
        if self.manager.move_to_group(item_id, group_id):
            log_info(f"已移动到分组 {group_id}", "Clipboard")
            self.load_history()
            return True
        return False

    def get_item_move_state(self, item_id: int, group_id: Optional[int]) -> tuple[bool, bool]:
        """获取分组内容是否可上移/下移"""
        if group_id is None:
            return False, False

        items = self.manager.get_by_group(group_id, offset=0, limit=1000)
        for index, item in enumerate(items):
            if item.id == item_id:
                return index > 0, index < len(items) - 1
        return False, False

    def move_item_order(self, item_id: int, group_id: Optional[int], direction: int) -> bool:
        """移动分组内容顺序（direction: -1 上移, 1 下移）"""
        if group_id is None:
            return False

        items = self.manager.get_by_group(group_id, offset=0, limit=1000)
        current_index = next((i for i, item in enumerate(items) if item.id == item_id), None)
        if current_index is None:
            return False

        new_index = current_index + (-1 if direction < 0 else 1)
        if new_index < 0 or new_index >= len(items):
            return False

        temp_items = [item for i, item in enumerate(items) if i != current_index]
        adjusted_new_index = new_index

        before_id = temp_items[adjusted_new_index - 1].id if adjusted_new_index > 0 else None
        after_id = temp_items[adjusted_new_index].id if adjusted_new_index < len(temp_items) else None

        if self.manager.move_item_between(item_id, before_id=before_id, after_id=after_id):
            if self.current_group_id == group_id:
                self.load_history()
            return True
        return False

    def open_manage_dialog_for_group(self, group_id: int,
                                     group_added_callback=None,
                                     data_changed_callback=None):
        """打开管理窗口并定位到分组编辑"""
        dialog = get_manage_dialog(self.manager)
        if group_added_callback:
            dialog.group_added.connect(group_added_callback, Qt.ConnectionType.UniqueConnection)
        if data_changed_callback:
            dialog.data_changed.connect(data_changed_callback, Qt.ConnectionType.UniqueConnection)
        dialog.open_group_editor(group_id)
        return dialog

    def open_manage_dialog_for_item(self, item_id: int, group_id: Optional[int],
                                    group_added_callback=None,
                                    data_changed_callback=None):
        """打开管理窗口并定位到内容编辑"""
        dialog = get_manage_dialog(self.manager)
        if group_added_callback:
            dialog.group_added.connect(group_added_callback, Qt.ConnectionType.UniqueConnection)
        if data_changed_callback:
            dialog.data_changed.connect(data_changed_callback, Qt.ConnectionType.UniqueConnection)
        dialog.open_item_editor(item_id, group_id)
        return dialog
    
    def clear_history(self, parent_widget=None) -> bool:
        """清空历史
        
        Args:
            parent_widget: 父窗口（用于显示确认对话框）
        
        Returns:
            是否清空成功
        """
        # 显示确认对话框
        if parent_widget:
            reply = show_confirm_dialog(
                parent_widget,
                "Confirm Clear",
                "Are you sure you want to clear all clipboard history?\nThis action cannot be undone."
            )
            if not reply:
                return False

        # 清空历史
        if self.manager.clear_history():
            self.load_history()
            return True
        return False
    
    def get_item(self, item_id: int) -> Optional[ClipboardItem]:
        """获取指定项目"""
        return self.manager.get_item(item_id)
    
    # ==================== 窗口状态管理 ====================
    
    def on_window_show(self):
        """窗口显示时调用"""
        # 记录当前前台窗口（在显示剪贴板窗口之前）
        self._previous_window_hwnd = get_foreground_window()
        # 重新加载数据
        self.load_history()
    
    def on_new_content(self, is_window_visible: bool):
        """新内容到达时调用
        
        Args:
            is_window_visible: 窗口是否可见
        """
        # 只在窗口可见时刷新
        if is_window_visible:
            self.load_history()

    # ==================== 侧边栏溢出 ====================

    def get_sidebar_overflow(
        self, bar_size: int, is_top: bool = False
    ) -> Tuple[List[Group], List[Group]]:
        """
        计算侧边栏可见分组和隐藏分组。

        :param bar_size: right_bar 的 height（竖向）或 width（top 模式）
        :param is_top:   True 表示横向 top 模式
        返回 (visible_groups, hidden_groups)
        """
        groups = self.get_groups()
        # 过滤掉隐藏类型分组（不在侧边栏显示）
        groups = [g for g in groups if g.group_type != GroupType.HIDDEN]
        if is_top:
            cap = calc_topbar_capacity(bar_size)
        else:
            cap = calc_sidebar_capacity(bar_size)

        if cap == -1 or len(groups) <= cap:
            return list(groups), []

        visible = list(groups[:cap])
        hidden = list(groups[cap:])

        # 确保当前选中分组可见
        current_gid = self.current_group_id
        if current_gid is not None and visible:
            visible_ids = {g.id for g in visible}
            if current_gid not in visible_ids:
                selected = next((g for g in hidden if g.id == current_gid), None)
                if selected:
                    hidden.remove(selected)
                    hidden.insert(0, visible[-1])
                    visible[-1] = selected

        return visible, hidden

    # ==================== 菜单数据组装 ====================

    def build_context_menu_data(self, item_id: int) -> Optional[ContextMenuData]:
        """组装右键菜单所需数据。返回 None 表示 item_id 无效。"""
        return self._context_menu_controller.build_context_menu_data(item_id)

    def build_group_context_menu_data(self, group_id: int) -> List[MenuAction]:
        """组装分组右键菜单数据"""
        return self._context_menu_controller.build_group_context_menu_data(group_id)
 
