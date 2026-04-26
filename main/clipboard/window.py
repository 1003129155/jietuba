# -*- coding: utf-8 -*-
"""
剪贴板历史窗口

提供的剪贴板历史管理界面。
"""

import ctypes
from PySide6.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout, QListWidget, QListWidgetItem,
    QLineEdit, QPushButton, QLabel, QMenu, QApplication,
    QFrame, QToolButton, QComboBox, QSizePolicy
)
from PySide6.QtCore import Qt, Signal, QSize, QTimer, QPoint, QEvent, QSettings
from PySide6.QtGui import QPixmap, QCursor, QColor
from time import perf_counter

from typing import Optional, List
from .data_manager import ClipboardManager, ClipboardItem
from .data_setting import ManageDialog, get_manage_dialog
from .preview_popup import PreviewPopup
from .data_controller import ClipboardController, get_foreground_window, set_foreground_window, send_ctrl_v
from .interaction import SelectionManager
from .frameless_mixin import FramelessMixin
from .item_delegate import ClipboardItemDelegate, ROLE_ITEM_DATA, ROLE_ITEM_ID
from .themes import get_theme_manager, Theme
from .theme_styles import ThemeStyleGenerator
from .group_bar import GroupBar
from core.logger import log_debug, log_error, log_exception
from core import safe_event
from core.shortcut_manager import ShortcutManager, ShortcutHandler


class ClipboardShortcutHandler(ShortcutHandler):
    """剪贴板窗口快捷键处理器 (priority=60)"""

    def __init__(self, window: 'ClipboardWindow'):
        self._window = window

    @property
    def priority(self) -> int:
        return 60

    @property
    def handler_name(self) -> str:
        return "ClipboardWindow"

    def is_active(self) -> bool:
        w = self._window
        try:
            return w is not None and w.isVisible()
        except RuntimeError:
            return False

    def handle_key(self, event) -> bool:
        w = self._window
        key = event.key()
        modifiers = event.modifiers()

        if key == Qt.Key.Key_Escape:
            # 有选中项时先取消选中，否则关闭窗口
            if (hasattr(w, 'selection_manager')
                    and w.selection_manager._selected_index >= 0):
                w.selection_manager.reset()
                return True
            w.close()
            return True

        if key in (Qt.Key.Key_Return, Qt.Key.Key_Enter):
            w._paste_selected()
            return True

        if key == Qt.Key.Key_Delete:
            w._delete_selected()
            return True

        if (key == Qt.Key.Key_F
                and modifiers == Qt.KeyboardModifier.ControlModifier):
            w.search_input.setFocus()
            return True

        # 数字键快捷选择 (1-9 → 索引 0-8)
        # 搜索框有焦点时放行，让用户正常输入
        if Qt.Key.Key_1 <= key <= Qt.Key.Key_9:
            focus_widget = w.search_input
            from PySide6.QtWidgets import QApplication as _App
            if _App.focusWidget() is not focus_widget:
                index = key - Qt.Key.Key_1
                items = w.controller.current_items
                if index < len(items):
                    w._on_paste_item(items[index].id)
                    return True
            return False

        # 字母键快捷选择 (a-z → 索引 9-34)
        # 搜索框有焦点时放行，让用户正常输入
        if Qt.Key.Key_A <= key <= Qt.Key.Key_Z:
            focus_widget = w.search_input
            from PySide6.QtWidgets import QApplication as _App
            if _App.focusWidget() is not focus_widget:
                index = 9 + (key - Qt.Key.Key_A)
                items = w.controller.current_items
                if index < len(items):
                    w._on_paste_item(items[index].id)
                    return True
            return False

        return False


class ClipboardWindow(QWidget, FramelessMixin):
    """
    剪贴板历史窗口
    
    显示剪贴板历史记录，支持搜索、筛选、分组等功能。
    """
    
    # 信号
    item_pasted = Signal(int)  # 粘贴项信号
    closed = Signal()  # 关闭信号
    new_item_received = Signal()  # 新内容信号（用于外部触发刷新）
    _offscreen_warmup_done = False
    
    def __init__(self, parent=None):
        super().__init__(parent)
        self.manager = ClipboardManager()
        
        # 创建控制器（统一的业务逻辑层）
        self.controller = ClipboardController(self.manager)
        
        # 连接控制器信号
        self.controller.data_loaded.connect(self._on_data_loaded)
        self.controller.loading_state_changed.connect(self._on_loading_changed)
        self.controller.reload_required.connect(self._on_reload_required)
        self.controller.load_completed.connect(self._on_load_completed)
        
        # UI 状态相关
        self.selected_item_id: Optional[int] = None
        self._is_loading = False  # UI 加载状态指示
        # 管理窗口触发的数据变更：仅在窗口可见时刷新
        self._ignore_manage_refresh_when_hidden = True
        # 自动填充上限（滚动条未出现时最多自动加载的页数）
        self._auto_fill_max_pages = 3
        self._auto_fill_remaining = self._auto_fill_max_pages
        self._offscreen_warmup_in_progress = False
        
        # 连接新内容信号到刷新方法
        self.new_item_received.connect(self._on_new_item)
        
        # 无边框窗口拖拽/调整大小（FramelessMixin）
        self._init_frameless(edge_margin=8)
        
        # 使用 QSettings 保存窗口位置和大小
        self._qsettings = QSettings("Jietuba", "ClipboardWindow")
        
        # 初始化主题管理器
        self.theme_manager = get_theme_manager()
        self.current_theme = self.theme_manager.get_current_theme()
        # 连接主题改变信号
        self.theme_manager.theme_changed.connect(self._on_theme_changed)
        self.theme_manager.font_size_changed.connect(self._on_font_size_changed)
        self.theme_manager.opacity_changed.connect(self._on_opacity_changed)
        
        # 加载设置
        self._load_settings()
        
        # 加载窗口位置和大小
        self._load_window_geometry()
        
        self._setup_ui()
        
        # UI 创建完成后，重新应用透明度（确保生效）
        self._apply_opacity()
        
        self._setup_shortcuts()
        # 初次加载由 showEvent 触发，这里不需要调用
        # self._load_history()
        
        # 设置预览弹窗的管理器
        PreviewPopup.instance().set_manager(self.manager)

        # 离屏弹出一次，预热窗口底层初始化（只执行一次）
        self._warmup_offscreen_once()
    
    # ==================== 控制器信号处理 ====================
    
    def _on_data_loaded(self, items: List[ClipboardItem], is_first_page: bool):
        """数据加载完成的处理"""
        if is_first_page:
            # 重置自动填充次数（每次加载第一页时）
            self._auto_fill_remaining = self._auto_fill_max_pages
            # 第一页：刷新整个列表
            if items or self.controller.current_items:
                self._refresh_list()
            else:
                # 没有数据，清空列表
                self.list_widget.clear()
        else:
            # 追加数据
            self._append_items(items)
    
    def _on_loading_changed(self, is_loading: bool):
        """加载状态变化"""
        self._is_loading = is_loading
        # 可以在这里添加加载指示器
    
    def _on_reload_required(self):
        """需要重新加载"""
        self.controller.load_history()
        self.group_bar.refresh_buttons()

    def _on_manage_data_changed(self):
        """管理窗口数据变化时回调（避免隐藏状态下触发大量加载）"""
        if self._ignore_manage_refresh_when_hidden and not self.isVisible():
            log_debug("管理窗口数据已变更，但剪贴板窗口不可见，延迟刷新", "Clipboard")
            return
        self._load_history()
    
    # ==================== 主题管理 ====================
    
    def _on_theme_changed(self, theme: Theme):
        """主题改变时的回调"""
        self.current_theme = theme
        self._apply_theme()
    
    def _apply_theme(self):
        """应用主题到所有 UI 元素"""
        # 更新 delegate 主题
        if hasattr(self, '_item_delegate'):
            self._item_delegate.set_theme(self.current_theme)
            self._item_delegate.set_window_opacity(self.window_opacity)
        # 应用透明度和主题
        self._apply_opacity()
        # 刷新侧边栏按钮样式
        self.group_bar.set_theme(self.current_theme)
        # 刷新底部栏控件样式（搜索框、齿轮、清除按钮）
        if hasattr(self, 'search_input'):
            has_text = bool(self.search_input.text().strip())
            self._apply_search_input_style(has_text)
            self._apply_clear_search_btn_style()
            self._apply_menu_btn_style()
        # 刷新列表项以应用新主题
        self._refresh_list()
        log_debug(f"主题已切换到: {self.current_theme.display_name}", "Clipboard")
    

    
    # ==================== 设置管理 ====================
    
    def _load_settings(self):
        """加载 UI 设置"""
        from settings import get_tool_settings_manager
        self.config = get_tool_settings_manager()
        # UI 设置:透明度和字体大小
        self.window_opacity = self.config.get_clipboard_window_opacity()
        # 使用字体大小配置
        self.display_lines = self.config.get_clipboard_font_size()
        # 分组栏位置
        self.group_bar_position = self.config.get_clipboard_group_bar_position()
    
    def _apply_opacity(self):
        """应用背景透明度和主题（不影响内容）"""
        # 使用主题样式生成器
        generator = ThemeStyleGenerator(self.current_theme)
        
        # 更新主容器背景色
        if hasattr(self, 'container'):
            self.container.setStyleSheet(generator.generate_window_style(self.window_opacity))
        
        # 更新列表背景色
        if hasattr(self, 'list_widget'):
            self.list_widget.setStyleSheet(generator.generate_list_widget_style(self.window_opacity))
        
        # 更新底部搜索栏背景色
        if hasattr(self, 'bottom_bar'):
            self.bottom_bar.setStyleSheet(generator.generate_search_bar_style(self.window_opacity))
        
        # 更新右侧按钮栏背景色
        if hasattr(self, 'group_bar') and self.group_bar.bar_widget is not None:
            border_dir = {"left": "border-right:", "top": "border-bottom:", "right": "border-left:"}
            bd = border_dir.get(getattr(self, 'group_bar_position', 'right'), "border-left:")
            self.group_bar.bar_widget.setStyleSheet(generator.generate_search_bar_style(self.window_opacity).replace(
                "border-top:", bd
            ))
    
    def _load_window_geometry(self):
        """加载窗口位置和大小"""
        try:
            # 从 tool_settings 获取默认值
            from settings import get_tool_settings_manager
            config = get_tool_settings_manager()
            default_width = config.get_app_setting("clipboard_window_width", 450)
            default_height = config.get_app_setting("clipboard_window_height", 600)
        except Exception as e:
            log_exception(e, "加载剪贴板窗口几何设置")
            default_width = 450
            default_height = 600
        
        # 从 QSettings 加载保存的值
        self._saved_x = self._qsettings.value("window/x", None)
        self._saved_y = self._qsettings.value("window/y", None)
        self._saved_width = self._qsettings.value("window/width", default_width, type=int)
        self._saved_height = self._qsettings.value("window/height", default_height, type=int)
        
        # 转换类型（QSettings 可能返回字符串）
        if self._saved_x is not None:
            self._saved_x = int(self._saved_x)
        if self._saved_y is not None:
            self._saved_y = int(self._saved_y)
    
    def _save_window_geometry(self):
        """保存窗口位置和大小"""
        self._qsettings.setValue("window/x", self.x())
        self._qsettings.setValue("window/y", self.y())
        self._qsettings.setValue("window/width", self.width())
        self._qsettings.setValue("window/height", self.height())
        self._qsettings.sync()
    
    def _setup_ui(self):
        """设置 UI - Ditto 风格简约布局"""
        self.setWindowTitle(self.tr("Clipboard History"))
        self.setWindowFlags(
            Qt.WindowType.Window |
            Qt.WindowType.FramelessWindowHint |
            Qt.WindowType.WindowStaysOnTopHint |
            Qt.WindowType.Tool  # 不在任务栏显示图标
        )
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.setMinimumSize(320, 400)
        
        # 设置窗口大小（从配置恢复或默认值）
        self.resize(self._saved_width, self._saved_height)
        
        # 主容器 - 简约白色风格
        self.container = QFrame(self)
        self.container.setStyleSheet("""
            QFrame#mainContainer {
                background: #FFFFFF;
                border: 1px solid #E0E0E0;
                border-radius: 4px;
            }
            QToolTip {
                background: #FFFFFF;
                color: #333333;
                border: 1px solid #E0E0E0;
                border-radius: 4px;
                padding: 4px 8px;
                font-size: 12px;
            }
        """)
        self.container.setObjectName("mainContainer")
        
        main_layout = QVBoxLayout(self)
        main_layout.setContentsMargins(0, 0, 0, 0)
        main_layout.addWidget(self.container)
        
        # 主内容布局：左侧内容 + 右侧按钮栏
        self.content_layout = QHBoxLayout(self.container)
        self.content_layout.setContentsMargins(0, 0, 0, 0)
        self.content_layout.setSpacing(0)
        
        # ========== 左侧主内容区 ==========
        self.left_widget = QWidget()
        self.left_widget.setStyleSheet("background: transparent;")
        self.left_layout = QVBoxLayout(self.left_widget)
        self.left_layout.setContentsMargins(0, 0, 0, 0)
        self.left_layout.setSpacing(0)
        
        # 历史列表
        self.list_widget = QListWidget()
        self.list_widget.setStyleSheet("""
            QListWidget {
                background: #FFFFFF;
                border: none;
                outline: none;
                color: #333333;
            }
            QListWidget::item {
                padding: 0px;
                border: none;
                background: transparent;
            }
            QListWidget::item:selected {
                background: transparent;  /* 透明,由 delegate 绘制选中效果 */
            }
            QListWidget::item:hover {
                background: transparent;  /* 透明,由 delegate 绘制悬停效果 */
            }
        """)
        self.list_widget.setSpacing(0)  # 列表项之间间距：2→0
        self.list_widget.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.list_widget.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAsNeeded)
        self.list_widget.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        self.list_widget.customContextMenuRequested.connect(self._show_context_menu)
        
        # 设置高性能绘制代理（替代 setItemWidget）
        show_metadata = self.config.get_clipboard_show_metadata()
        try:
            line_height_padding = self.config.get_clipboard_line_height_padding()
        except Exception as e:
            log_exception(e, "获取行高填充设置")
            line_height_padding = 8
        self._item_delegate = ClipboardItemDelegate(
            parent=self.list_widget,
            theme=self.current_theme,
            display_lines=self.display_lines,
            window_opacity=self.window_opacity,
            show_metadata=show_metadata,
            line_height_padding=line_height_padding,
        )
        self.list_widget.setItemDelegate(self._item_delegate)
        # 启用鼠标追踪，让 hover 事件生效
        self.list_widget.setMouseTracking(True)
        self.list_widget.viewport().setMouseTracking(True)
        # 连接 hover 信号（delegate 模式下替代 widget 的 enterEvent/leaveEvent）
        self.list_widget.itemEntered.connect(self._on_delegate_item_entered)
        self.list_widget.viewport().installEventFilter(self)
        
        # 初始化选择管理器（统一管理键盘和鼠标交互）
        self.selection_manager = SelectionManager(self.list_widget, self._get_item_data)
        self.selection_manager.group_bar_position = self.group_bar_position
        self.selection_manager.item_activated.connect(self._on_paste_item)
        self.selection_manager.request_sidebar_focus.connect(self._enter_sidebar_mode)
        self.selection_manager.request_group_switch.connect(self._on_top_group_switch)
        
        # 监听选中状态变化,更新 widget 的选中样式
        self.list_widget.itemSelectionChanged.connect(self._on_selection_changed)
        
        # 连接滚动条信号，实现滚动加载和宽度更新
        scrollbar = self.list_widget.verticalScrollBar()
        scrollbar.valueChanged.connect(self._on_scroll)
        scrollbar.rangeChanged.connect(self._on_scrollbar_range_changed)  # 滚动条出现/消失时更新宽度
        
        self.left_layout.addWidget(self.list_widget, 1)
        
        # 底部搜索栏
        self.bottom_bar = QWidget()
        self.bottom_bar.setFixedHeight(36)
        self.bottom_bar.setStyleSheet("""
            QWidget {
                background: #FAFAFA;
                border-top: 1px solid #E0E0E0;
            }
        """)
        bottom_layout = QHBoxLayout(self.bottom_bar)
        bottom_layout.setContentsMargins(8, 4, 8, 4)
        bottom_layout.setSpacing(8)
        
        # 搜索图标
        search_icon = QLabel("🔍")
        search_icon.setStyleSheet("background: transparent; border: none;")
        bottom_layout.addWidget(search_icon)
        
        # 搜索输入框
        self.search_input = QLineEdit()
        self.search_input.setPlaceholderText(self.tr("Search"))
        self._apply_search_input_style()
        self.search_input.textChanged.connect(self._on_search_changed)
        self.search_input.textChanged.connect(self._update_search_background)
        bottom_layout.addWidget(self.search_input, 1)
        # 让搜索框的方向键也能导航列表
        self.selection_manager.set_search_input(self.search_input)
        
        # 清除搜索按钮
        self.clear_search_btn = QPushButton("×")
        self.clear_search_btn.setFixedSize(24, 24)
        self.clear_search_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        self.clear_search_btn.setToolTip(self.tr("Clear search"))
        self.clear_search_btn.setFocusPolicy(Qt.FocusPolicy.NoFocus)  # 不接受焦点
        self._apply_clear_search_btn_style()
        self.clear_search_btn.clicked.connect(self._clear_search)
        self.clear_search_btn.hide()  # 初始隐藏
        bottom_layout.addWidget(self.clear_search_btn)
        
        # 隐藏的类型筛选（保留功能但不显示）
        self.type_filter = QComboBox()
        self.type_filter.addItems([self.tr("All"), self.tr("Text"), self.tr("Image"), self.tr("File")])
        self.type_filter.hide()
        
        # 设置菜单按钮（齿轮）
        self.menu_btn = QPushButton("⚙")
        self.menu_btn.setFixedSize(28, 28)
        self.menu_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        self.menu_btn.setToolTip(self.tr("Settings"))
        self.menu_btn.setFocusPolicy(Qt.FocusPolicy.NoFocus)  # 不接受焦点
        self._apply_menu_btn_style()
        self.menu_btn.clicked.connect(self._show_main_menu)
        bottom_layout.addWidget(self.menu_btn)
        
        self.left_layout.addWidget(self.bottom_bar)
        
        self.content_layout.addWidget(self.left_widget, 1)
        
        # ========== 分组栏组件 ==========
        self.group_bar = GroupBar(
            controller=self.controller,
            manager=self.manager,
            theme=self.current_theme,
            position=self.group_bar_position,
            parent=self,
        )
        self.group_bar.close_requested.connect(self.close)
        self.group_bar.group_switched.connect(self._on_group_switched)
        self.group_bar.sidebar_entered.connect(self._on_sidebar_entered)
        self.group_bar.sidebar_exited.connect(self._on_sidebar_exited)
        self.group_bar.manage_groups_requested.connect(self._on_add_group_clicked)
        self.group_bar.manage_items_requested.connect(self._on_add_item_clicked)
        self.group_bar.build(self.content_layout, self.left_widget, self.left_layout)
        
        # 为所有子控件启用鼠标追踪和事件过滤器，以便检测边缘
        self._setup_mouse_tracking_recursive(self)
    
    def _get_menu_style(self):
        """获取菜单样式（基于当前主题）"""
        return ThemeStyleGenerator(self.current_theme).generate_menu_style()
    
    def _show_main_menu(self):
        """显示/关闭主菜单（齿轮按钮）- toggle 行为"""
        # 如果菜单已经打开，关闭它
        if hasattr(self, '_settings_menu') and self._settings_menu is not None and self._settings_menu.isVisible():
            self._settings_menu.close()
            self._settings_menu = None
            return
        
        # 防止菜单被 Qt 自动关闭后又立即重新打开（闪烁问题）
        if hasattr(self, '_menu_close_time') and (perf_counter() - self._menu_close_time) < 0.3:
            return
        
        from .setting_panel import show_setting_menu
        self._settings_menu = show_setting_menu(
            parent=self,
            menu_style=self._get_menu_style(),
            tr=self.tr,
            current_filter_index=self.type_filter.currentIndex(),
            paste_with_html=self.controller.paste_with_html,
            auto_paste=self.config.get_clipboard_auto_paste(),
            move_to_top=self.config.get_clipboard_move_to_top_on_paste(),
            show_metadata=self.config.get_clipboard_show_metadata(),
            preserve_search=self.config.get_clipboard_preserve_search(),
            window_opacity=self.window_opacity,
            current_font_size=self.config.get_clipboard_font_size(),
            current_theme_name=self.theme_manager.get_current_theme().name,
            current_group_bar_position=self.group_bar_position,
            opacity_options=self.config.get_clipboard_window_opacity_options(),
            font_size_options=self.config.get_clipboard_font_size_options(),
            on_set_filter=self._set_filter,
            on_toggle_paste_html=self._toggle_paste_with_html,
            on_toggle_auto_paste=self._toggle_auto_paste,
            on_toggle_move_to_top=self._toggle_move_to_top_on_paste,
            on_toggle_show_metadata=self._toggle_show_metadata,
            on_toggle_preserve_search=self._toggle_preserve_search,
            on_set_opacity=self._set_window_opacity,
            on_set_font_size=self._set_font_size,
            on_set_theme=self._set_theme,
            on_add_item=self._on_add_item_clicked,
            on_set_group_bar_position=self._set_group_bar_position,
            anchor_pos=self.menu_btn.mapToGlobal(QPoint(0, 0)),
        )
        # 菜单关闭时记录时间并清理引用
        def _on_menu_hide():
            self._menu_close_time = perf_counter()
            self._settings_menu = None
        self._settings_menu.aboutToHide.connect(_on_menu_hide)
    
    def _set_filter(self, index: int):
        """设置筛选类型（同步 UI 下拉框 + 委托 controller）"""
        self.type_filter.setCurrentIndex(index)
        self.controller.set_content_type_filter(index)

    def _toggle_paste_with_html(self, checked: bool):
        self.controller.set_paste_with_html(checked)

    def _toggle_auto_paste(self, checked: bool):
        self.controller.set_auto_paste(checked)

    def _toggle_move_to_top_on_paste(self, checked: bool):
        self.controller.set_move_to_top_on_paste(checked)

    def _toggle_show_metadata(self, checked: bool):
        self.config.set_clipboard_show_metadata(checked)
        if hasattr(self, '_item_delegate'):
            self._item_delegate.set_show_metadata(checked)
        self.controller.load_history()

    def _toggle_preserve_search(self, checked: bool):
        self.config.set_clipboard_preserve_search(checked)

    def _set_window_opacity(self, percent: int):
        """设置窗口透明度"""
        self.window_opacity = percent
        self.config.set_clipboard_window_opacity(percent)
        if hasattr(self, '_item_delegate'):
            self._item_delegate.set_window_opacity(percent)
        self._apply_opacity()
        self._refresh_list()

    def _set_font_size(self, size: int):
        """设置字体大小"""
        self.display_lines = size
        self.config.set_clipboard_font_size(size)
        if hasattr(self, '_item_delegate'):
            self._item_delegate.set_display_lines(size)
        self._refresh_list()

    def _set_theme(self, theme_name: str):
        get_theme_manager().set_theme(theme_name)

    def _on_font_size_changed(self, size: int):
        """外部（设置页面）通知字体大小变更"""
        self.display_lines = size
        if hasattr(self, '_item_delegate'):
            self._item_delegate.set_display_lines(size)
        self._refresh_list()

    def _on_opacity_changed(self, percent: int):
        """外部（设置页面）通知透明度变更"""
        self.window_opacity = percent
        if hasattr(self, '_item_delegate'):
            self._item_delegate.set_window_opacity(percent)
        self._apply_opacity()
        self._refresh_list()

    def _setup_shortcuts(self):
        """创建快捷键处理器（注册/注销由 showEvent/closeEvent 管理）"""
        self._shortcut_handler = ClipboardShortcutHandler(self)
    
    def _load_history(self):
        """加载历史记录（重置并加载第一页）- 委托给 controller"""
        self.controller.load_history()
    
    def _load_more_items(self):
        """加载更多项目（分页加载）- 委托给 controller"""
        self.controller._load_more_items()
    
    def _refresh_list(self):
        """刷新列表显示 — 使用 delegate 绘制，只创建 QListWidgetItem（无 widget 开销）"""
        from time import perf_counter
        t0 = perf_counter()
        
        # 关闭预览窗口并重置选中状态
        PreviewPopup.instance().hide_preview()
        self.selection_manager.reset()
        
        # 暂停 UI 更新
        self.list_widget.setUpdatesEnabled(False)
        self.list_widget.clear()
        
        # 同步 delegate 配置
        show_metadata = self.config.get_clipboard_show_metadata()
        self._item_delegate.set_show_metadata(show_metadata)
        self._item_delegate.set_display_lines(self.display_lines)
        self._item_delegate.set_window_opacity(self.window_opacity)
        self._item_delegate.set_theme(self.current_theme)
        self._item_delegate.set_highlighted_id(None)
        
        t1 = perf_counter()
        
        # 使用 controller 的 current_items — 只创建 QListWidgetItem + 存数据
        for item in self.controller.current_items:
            list_item = QListWidgetItem()
            list_item.setData(ROLE_ITEM_ID, item.id)
            list_item.setData(ROLE_ITEM_DATA, item)
            self.list_widget.addItem(list_item)
        
        t2 = perf_counter()
        
        # 恢复 UI 更新，一次性重绘
        self.list_widget.setUpdatesEnabled(True)
        self.list_widget.scrollToTop()
        self.list_widget.update()
        
        t3 = perf_counter()
    
    def _on_load_completed(self):
        """单次加载完全完成后的回调（此时 _is_loading 已经是 False）"""
        # 延迟到下一个事件循环，让布局自然完成，避免 processEvents 阻塞
        QTimer.singleShot(0, self._check_and_load_more_if_needed)
    
    def _check_and_load_more_if_needed(self):
        """检查是否需要加载更多数据（如果没有滚动条则继续加载）"""
        scrollbar = self.list_widget.verticalScrollBar()
        is_visible = scrollbar.isVisible()
        maximum = scrollbar.maximum()
        has_more = self.controller.has_more_items()
        
        # 如果滚动条不可见或最大值为0，说明内容不够填满窗口
        if not is_visible or maximum == 0:
            # 检查是否还有更多数据可以加载
            if has_more:
                if self._auto_fill_remaining <= 0:
                    return
                self._auto_fill_remaining -= 1
                # 触发加载，load_completed 信号会再次调用本方法（通过 _on_load_completed）
                self.controller._load_more_items()
            else:
                pass
        else:
            pass
    
    def _append_items(self, items: List[ClipboardItem]):
        """追加项目到列表末尾（用于分页加载）— delegate 模式"""
        log_debug(f"📝 _append_items 被调用，准备追加 {len(items)} 条数据", "Clipboard")
        
        # 暂停 UI 更新
        self.list_widget.setUpdatesEnabled(False)
        
        for item in items:
            list_item = QListWidgetItem()
            list_item.setData(ROLE_ITEM_ID, item.id)
            list_item.setData(ROLE_ITEM_DATA, item)
            self.list_widget.addItem(list_item)
        
        # 恢复 UI 更新
        self.list_widget.setUpdatesEnabled(True)
        self.list_widget.update()
    
    def _on_scroll(self, value: int):
        """滚动条值变化时检查是否需要加载更多 - 委托给 controller"""
        scrollbar = self.list_widget.verticalScrollBar()
        maximum = scrollbar.maximum()
        self.controller.check_scroll_load(value, maximum)
    
    def _on_scrollbar_range_changed(self, min_val: int, max_val: int):
        """滚动条范围变化时触发重绘（delegate 模式不需要逐项调宽度）"""
        self.list_widget.viewport().update()
    
    def _on_search_changed(self, text: str):
        """搜索文本变化"""
        # 延迟搜索，避免频繁查询
        if hasattr(self, '_search_timer'):
            self._search_timer.stop()
        else:
            self._search_timer = QTimer()
            self._search_timer.setSingleShot(True)
            self._search_timer.timeout.connect(lambda: self.controller.set_search_text(self.search_input.text()))
        
        self._search_timer.start(300)
    
    def _on_selection_changed(self):
        """列表选中状态变化时(键盘导航),更新高亮"""
        selected_items = self.list_widget.selectedItems()
        if selected_items:
            selected_id = selected_items[0].data(Qt.ItemDataRole.UserRole)
            self._set_highlighted_item(selected_id)
        else:
            self._set_highlighted_item(None)
    
    def _on_delegate_item_entered(self, list_item: QListWidgetItem):
        """delegate 模式: 鼠标进入某项"""
        item_id = list_item.data(ROLE_ITEM_ID)
        if item_id is None:
            return
        self._set_highlighted_item(item_id)
        
        # 记录悬停索引
        row = self.list_widget.row(list_item)
        if row >= 0:
            self.selection_manager.set_hovered_index(row)
        
        # 触发预览
        item_data = list_item.data(ROLE_ITEM_DATA)
        if item_data:
            popup = PreviewPopup.instance()
            pos = QCursor.pos()
            popup.show_preview(item_data, pos, delay_ms=5)

    def _set_highlighted_item(self, item_id: Optional[int]):
        """统一设置高亮项(只能有一个) — delegate 模式只需更新 id + 重绘"""
        if self._item_delegate._highlighted_id != item_id:
            self._item_delegate.set_highlighted_id(item_id)
            self.list_widget.viewport().update()

    def _update_search_background(self, text: str):
        """根据搜索框内容更新背景色和清除按钮显示状态"""
        if text.strip():
            self._apply_search_input_style(has_text=True)
            self.clear_search_btn.show()
        else:
            self._apply_search_input_style(has_text=False)
            self.clear_search_btn.hide()
    
    def _apply_search_input_style(self, has_text: bool = False):
        """根据主题和搜索状态生成搜索框样式"""
        style = ThemeStyleGenerator(self.current_theme).generate_search_input_style(has_text)
        self.search_input.setStyleSheet(style)
    
    def _apply_clear_search_btn_style(self):
        """根据主题生成清除搜索按钮样式"""
        style = ThemeStyleGenerator(self.current_theme).generate_clear_search_btn_style()
        self.clear_search_btn.setStyleSheet(style)
    
    def _apply_menu_btn_style(self):
        """根据主题生成齿轮按钮样式"""
        style = ThemeStyleGenerator(self.current_theme).generate_menu_btn_style()
        self.menu_btn.setStyleSheet(style)
    
    def _clear_search(self):
        """清除搜索内容"""
        self.search_input.clear()
        self.search_input.setFocus()
    
    def _on_filter_changed(self, index: int):
        """类型筛选变化 - 委托给 controller"""
        self.controller.set_content_type_filter(index)
    
    # ==================== 分组功能（委托给 GroupBar） ====================

    def _on_group_switched(self, group_id):
        """GroupBar 信号: 用户切换了分组"""
        self.group_bar.switch_to_group(group_id)

    def _on_sidebar_entered(self):
        """GroupBar 信号: 进入侧栏导航模式"""
        self.selection_manager.clear_selection()
        self._set_highlighted_item(None)
        self.list_widget.setFocus()

    def _on_sidebar_exited(self, prev_item_id):
        """GroupBar 信号: 退出侧栏导航模式"""
        self.list_widget.setFocus()
        if prev_item_id is not None:
            self.selection_manager.select_item_id(prev_item_id)
        else:
            self.selection_manager._move_selection(1)

    def _enter_sidebar_mode(self):
        """SelectionManager 信号 → 委托给 GroupBar"""
        prev_id = self.selection_manager.get_current_item_id()
        self.group_bar.enter_sidebar_mode(prev_id)

    def _on_top_group_switch(self, delta: int):
        """SelectionManager 信号 → 委托给 GroupBar"""
        self.group_bar.handle_top_group_switch(delta)

    def _on_add_group_clicked(self):
        dialog = get_manage_dialog(self.manager)
        self._connect_manage_dialog(dialog)
        dialog.show_and_activate()

    def _on_add_item_clicked(self):
        dialog = get_manage_dialog(self.manager)
        self._connect_manage_dialog(dialog)
        dialog._switch_page(1)
        dialog.show_and_activate()

    def _connect_manage_dialog(self, dialog):
        """安全连接管理窗口信号（避免重复连接）"""
        for sig, slot in [
            (dialog.group_added, self.group_bar.refresh_buttons),
            (dialog.data_changed, self._on_manage_data_changed),
        ]:
            sig.connect(slot, Qt.ConnectionType.UniqueConnection)

    def _set_group_bar_position(self, position: str):
        """设置分组栏位置（委托给 GroupBar + 保存配置）"""
        self.group_bar_position = position
        self.selection_manager.group_bar_position = position
        self.group_bar.set_position(position)
        self.config.set_clipboard_group_bar_position(position)
        self._apply_opacity()

    # ==================== 列表操作 ====================

    def _get_item_data(self, item_id: int) -> Optional[ClipboardItem]:
        for item in self.controller.current_items:
            if item.id == item_id:
                return item
        return None

    def _on_paste_item(self, item_id: int):
        self.controller._previous_window_hwnd = get_foreground_window()
        if self.controller.paste_item(item_id, on_close_callback=self.close):
            self.item_pasted.emit(item_id)

    def _paste_selected(self):
        item_id = self.selection_manager.get_current_item_id()
        if item_id:
            self._on_paste_item(item_id)

    def _delete_selected(self):
        item_id = self.selection_manager.get_current_item_id()
        if item_id:
            self.controller.delete_item(item_id)

    def _show_context_menu(self, pos):
        """显示右键菜单"""
        item = self.list_widget.itemAt(pos)
        if not item:
            return
        item_id = item.data(Qt.ItemDataRole.UserRole)
        if not item_id:
            return

        ctx = self.controller.build_context_menu_data(item_id)
        if ctx is None:
            return

        menu = QMenu(self)
        menu.setStyleSheet(self._get_menu_style())

        # 动作回调映射
        def _build_move_submenu(ad):
            sub = menu.addMenu(self.tr(ad.label))
            sub.setStyleSheet(self._get_menu_style())
            for child in ad.children:
                if child.is_separator:
                    sub.addSeparator()
                elif child.key.startswith("move_to_group_"):
                    gid = int(child.key.split("_")[-1])
                    a = sub.addAction(child.label)
                    a.triggered.connect(lambda _c, g=gid: self._move_item_to_group(item_id, g))
                elif child.key == "remove_from_group":
                    a = sub.addAction(self.tr(child.label))
                    a.triggered.connect(lambda: self._move_item_to_group(item_id, None))

        _item_handlers = {
            "paste":          lambda: self._on_paste_item(item_id),
            "pin_image":      lambda: self._create_pin_window(item_id),
            "toggle_pin":     lambda: self._toggle_pin(item_id),
            "open_file_location": lambda: self._open_file_location(item_id),
            "edit_item":      lambda: self._edit_item(item_id),
            "move_item_up":   lambda: self._move_item_order(item_id, -1),
            "move_item_down": lambda: self._move_item_order(item_id, 1),
            "delete_item":    lambda: self._delete_item(item_id),
        }

        for ad in ctx.actions:
            if ad.is_separator:
                menu.addSeparator()
            elif ad.key == "move_group_menu":
                _build_move_submenu(ad)
            else:
                act = menu.addAction(self.tr(ad.label))
                act.setEnabled(ad.enabled)
                if ad.checkable:
                    act.setCheckable(True)
                    act.setChecked(ad.checked)
                handler = _item_handlers.get(ad.key)
                if handler:
                    act.triggered.connect(handler)

        menu.exec(self.list_widget.mapToGlobal(pos))

    def _move_item_to_group(self, item_id: int, group_id: Optional[int]):
        self.controller.move_to_group(item_id, group_id)

    def _move_item_order(self, item_id: int, direction: int):
        self.controller.move_item_order(item_id, self.controller.current_group_id, direction)

    def _edit_item(self, item_id: int):
        self.controller.open_manage_dialog_for_item(
            item_id,
            self.controller.current_group_id,
            group_added_callback=self.group_bar.refresh_buttons,
            data_changed_callback=self._on_manage_data_changed,
        )

    def _toggle_pin(self, item_id: int):
        self.controller.toggle_pin(item_id)

    def _create_pin_window(self, item_id: int):
        from .pin_window import create_pin_from_clipboard_item
        create_pin_from_clipboard_item(item_id, self.controller, self)

    def _delete_item(self, item_id: int):
        self.controller.delete_item(item_id)

    def _open_file_location(self, item_id: int):
        """打开文件所在位置并选中文件（仅限 file 类型）"""
        import os, json, subprocess
        clipboard_item = self.controller.get_item(item_id)
        if clipboard_item is None or clipboard_item.content_type != "file":
            return
        try:
            data = json.loads(clipboard_item.content)
            files = data.get("files", [])
            if not files:
                return
            # 取第一个文件路径
            file_path = os.path.normpath(files[0])
            if os.path.exists(file_path):
                subprocess.Popen(["explorer", "/select,", file_path])
            elif os.path.exists(os.path.dirname(file_path)):
                # 文件已被删除，但目录还在，打开目录
                subprocess.Popen(["explorer", os.path.dirname(file_path)])
        except Exception as e:
            from core.logger import log_warning
            log_warning(f"打开文件位置失败: {e}", "Clipboard")

    def _on_clear_clicked(self):
        self.controller.clear_history(parent_widget=self)

    def _on_new_item(self):
        self.controller.on_new_content(self.isVisible())

    def notify_new_content(self):
        self.new_item_received.emit()

    def _warmup_offscreen_once(self):
        """将窗口离屏显示一次后隐藏，预热原生窗口状态（仅一次）。"""
        if ClipboardWindow._offscreen_warmup_done:
            return
        ClipboardWindow._offscreen_warmup_done = True

        screen = QApplication.primaryScreen()
        if screen is None:
            return

        self._offscreen_warmup_in_progress = True
        old_pos = self.pos()
        old_opacity = self.windowOpacity()
        try:
            geo = screen.availableGeometry()
            self.setWindowOpacity(0)
            self.move(geo.right() + 20000, geo.bottom() + 20000)
            self.show()
            QApplication.processEvents()
            self.hide()
            log_debug("剪贴板窗口离屏预热完成", "Clipboard")
        except Exception as e:
            log_debug(f"剪贴板窗口离屏预热异常: {e}", "Clipboard")
        finally:
            self.move(old_pos)
            self.setWindowOpacity(old_opacity)
            self._offscreen_warmup_in_progress = False

    @safe_event
    def showEvent(self, event):
        """显示时刷新"""
        if self._offscreen_warmup_in_progress:
            super().showEvent(event)
            return

        # 先隐藏窗口内容（opacity=0），避免低配机器上黑色底图闪现
        self.setWindowOpacity(0)
        
        # 注册快捷键处理器
        if hasattr(self, '_shortcut_handler') and self._shortcut_handler:
            ShortcutManager.instance().register(self._shortcut_handler)
        
        # 重置拖拽/调整大小状态，避免上次隐藏时残留的状态
        self._fl_reset()
        
        # 重置选择管理器（呼出时不选中任何项）
        self.selection_manager.reset()
        
        # 通知 controller
        self.controller.on_window_show()
        
        t_show_start = perf_counter()
        super().showEvent(event)
        
        # 设置焦点到列表控件,确保键盘导航可用
        self.list_widget.setFocus()
        
        # 每次显示都定位到鼠标位置（右下方）
        self._position_at_cursor()
        # 延迟重算分组按钮溢出（窗口布局稳定后高度才准确）
        QTimer.singleShot(0, self.group_bar.refresh_buttons)
        # 等内容绘制完成后再显示窗口，消除黑色底图闪现
        QTimer.singleShot(0, self._reveal_window)
        t_show_end = perf_counter()
        log_debug(f"⏱️ 打开窗口耗时: {(t_show_end - t_show_start) * 1000:.1f} ms", "Clipboard")
    
    def _reveal_window(self):
        """内容绘制完成后显示窗口"""
        self.setWindowOpacity(1)
    
    def _position_at_cursor(self):
        """将窗口定位到鼠标光标的右下方（第四象限）"""
        cursor_pos = QCursor.pos()
        screen = QApplication.screenAt(cursor_pos)
        if screen:
            screen_geo = screen.availableGeometry()
            
            # 窗口左上角位于鼠标右下方，留一点偏移
            x = cursor_pos.x() + 10
            y = cursor_pos.y() + 10
            
            # 如果右边放不下，改为放在鼠标左边
            if x + self.width() > screen_geo.right():
                x = cursor_pos.x() - self.width() - 10
            
            # 如果下边放不下，改为放在鼠标上边
            if y + self.height() > screen_geo.bottom():
                y = cursor_pos.y() - self.height() - 10
            
            # 确保不超出屏幕左边和上边
            if x < screen_geo.left():
                x = screen_geo.left()
            if y < screen_geo.top():
                y = screen_geo.top()
            
            self.move(x, y)
    
    @safe_event
    def hideEvent(self, event):
        """隐藏时保存位置和大小，并关闭预览窗口"""
        if self._offscreen_warmup_in_progress:
            super().hideEvent(event)
            return

        # 重置拖拽/调整大小状态，避免下次呼出时仍处于拖拽状态
        self._fl_reset()

        # 根据设置决定是否清空搜索框
        if not self.config.get_clipboard_preserve_search() and self.search_input.text():
            self.search_input.clear()
        
        super().hideEvent(event)
        self._save_window_geometry()
        # 关闭预览弹窗
        PreviewPopup.instance().hide_preview()
        # 关闭所有活动的菜单/弹窗
        active_popup = QApplication.activePopupWidget()
        if active_popup is not None:
            active_popup.close()
    
    @safe_event
    def closeEvent(self, event):
        """关闭事件"""
        # 注销快捷键处理器（下次 showEvent 会重新注册）
        if hasattr(self, '_shortcut_handler') and self._shortcut_handler:
            ShortcutManager.instance().unregister(self._shortcut_handler)
        self._save_window_geometry()
        # 关闭预览弹窗
        PreviewPopup.instance().hide_preview()
        # 关闭所有活动的菜单/弹窗
        active_popup = QApplication.activePopupWidget()
        if active_popup is not None:
            active_popup.close()
        self.closed.emit()
        super().closeEvent(event)
    
    @safe_event
    def changeEvent(self, event):
        """监听窗口激活状态变化"""
        super().changeEvent(event)
        if event.type() == QEvent.Type.ActivationChange:
            # 窗口失去激活状态（失去焦点）
            if not self.isActiveWindow():
                # 延迟检查，避免误触发
                QTimer.singleShot(100, self._check_and_hide)
    
    def _check_and_hide(self):
        """检查并隐藏窗口"""
        # 如果窗口不是激活状态，则隐藏
        if not self.isActiveWindow():
            self.hide()
    
    # ================ 窗口拖动和调整大小 ================

    def _is_draggable_area(self, widget, local_pos: QPoint) -> bool:
        """重写 FramelessMixin 钩子：仅分组栏空白处可拖动"""
        bar = getattr(self, 'group_bar', None)
        if bar and bar.bar_widget is not None and widget is bar.bar_widget:
            return True
        return False

    @safe_event
    def eventFilter(self, obj, event):
        """统一事件过滤器：键盘导航 + 拖拽/resize（委托 FramelessMixin）+ viewport hover"""
        event_type = event.type()

        # ── viewport leave: 鼠标离开列表区域 ──
        if event_type == QEvent.Type.Leave and obj is self.list_widget.viewport():
            self.selection_manager.clear_hovered_index()
            PreviewPopup.instance().hide_preview()
            selected_items = self.list_widget.selectedItems()
            if selected_items:
                selected_id = selected_items[0].data(Qt.ItemDataRole.UserRole)
                self._set_highlighted_item(selected_id)
            else:
                self._set_highlighted_item(None)

        # ── 焦点切入列表时退出侧边栏模式 ──
        if event_type == QEvent.Type.FocusIn and obj is self.list_widget:
            if not self.group_bar.sidebar_mode:
                self.group_bar.clear_sidebar_focus()

        # ── 键盘导航（方向自适应） ──
        if event_type == QEvent.Type.KeyPress:
            key = event.key()
            pos = getattr(self, 'group_bar_position', 'right')

            if pos != "top":
                if pos == "right":
                    k_enter, k_exit = Qt.Key.Key_Right, Qt.Key.Key_Left
                else:
                    k_enter, k_exit = Qt.Key.Key_Left, Qt.Key.Key_Right
                k_prev, k_next = Qt.Key.Key_Up, Qt.Key.Key_Down

                if self.group_bar.sidebar_mode:
                    if key == k_exit:
                        self.group_bar.exit_sidebar_mode()
                        return True
                    if key == k_prev:
                        self.group_bar.move_sidebar_selection(-1)
                        return True
                    if key == k_next:
                        self.group_bar.move_sidebar_selection(1)
                        return True
                    if key == k_enter:
                        return True
                else:
                    if key == k_enter:
                        focus_widget = QApplication.focusWidget()
                        if (
                            focus_widget is self.list_widget
                            or self.list_widget.isAncestorOf(focus_widget)
                        ):
                            self._enter_sidebar_mode()
                            return True

        # ── 拖拽 / resize（委托 FramelessMixin） ──
        if self._fl_handle_event(obj, event):
            return True

        return super().eventFilter(obj, event)

    @safe_event
    def leaveEvent(self, event):
        """鼠标离开窗口时重置光标"""
        if not self._fl_is_dragging and not self._fl_resize_edge:
            self.unsetCursor()
        super().leaveEvent(event)

    @safe_event
    def keyPressEvent(self, event):
        """键盘事件"""
        key = event.key()

        # 兜底：当焦点不在列表时，仍允许上下方向键切换列表项
        if key in (Qt.Key.Key_Up, Qt.Key.Key_Down) and not self.group_bar.sidebar_mode:
            focus_widget = QApplication.focusWidget()
            if focus_widget is None or focus_widget is self.search_input or not (
                focus_widget is self.list_widget or self.list_widget.isAncestorOf(focus_widget)
            ):
                self.list_widget.setFocus()
                self.selection_manager._move_selection(-1 if key == Qt.Key.Key_Up else 1)
                return

        super().keyPressEvent(event)

    @safe_event
    def resizeEvent(self, event):
        """窗口大小改变时触发重绘，并重算分组按钮溢出"""
        super().resizeEvent(event)
        # delegate 模式下只需触发重绘，无需逐项调宽度
        self.list_widget.viewport().update()
        # 重算分组按钮是否需要折叠
        if hasattr(self, 'group_bar') and self.group_bar.bar_widget is not None:
            self.group_bar.refresh_buttons()

 