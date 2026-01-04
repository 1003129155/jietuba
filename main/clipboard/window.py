# -*- coding: utf-8 -*-
"""
剪贴板历史窗口

提供类似 Ditto 的剪贴板历史管理界面。
"""

import ctypes
from PyQt6.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout, QListWidget, QListWidgetItem,
    QLineEdit, QPushButton, QLabel, QMenu, QMessageBox, QApplication,
    QFrame, QToolButton, QComboBox, QSizePolicy
)
from PyQt6.QtCore import Qt, pyqtSignal, QSize, QTimer, QPoint, QEvent, QSettings
from PyQt6.QtGui import QPixmap, QKeySequence, QShortcut, QCursor
from time import perf_counter

from typing import Optional, List
from .manager import ClipboardManager, ClipboardItem, Group
from .manage_dialog import ManageDialog
from .preview_popup import PreviewPopup


# Windows API 常量
VK_CONTROL = 0x11
VK_V = 0x56
KEYEVENTF_KEYUP = 0x0002


def get_foreground_window():
    """获取当前前台窗口句柄"""
    try:
        return ctypes.windll.user32.GetForegroundWindow()
    except Exception:
        return None


def set_foreground_window(hwnd):
    """设置前台窗口"""
    try:
        if hwnd:
            ctypes.windll.user32.SetForegroundWindow(hwnd)
            return True
    except Exception:
        pass
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
        print(f"❌ [Clipboard] 发送 Ctrl+V 失败: {e}")
        return False


class ClipboardItemWidget(QFrame):
    """剪贴板项显示组件"""
    
    clicked = pyqtSignal(int)  # 点击信号，传递 item_id
    double_clicked = pyqtSignal(int)  # 双击信号
    
    def __init__(self, item: ClipboardItem, display_lines: int = 1, parent=None):
        super().__init__(parent)
        self.item = item
        self.display_lines = display_lines  # 显示行数：1, 2, 3
        
        # 从配置中读取行高边距
        try:
            from settings import get_tool_settings_manager
            config = get_tool_settings_manager()
            self.line_height_padding = config.get_clipboard_line_height_padding()
        except Exception:
            self.line_height_padding = 8  # 默认值
        
        # 设置尺寸策略：水平方向扩展，垂直方向固定
        from PyQt6.QtWidgets import QSizePolicy
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        
        self._setup_ui()
    
    def _setup_ui(self):
        """设置 UI"""
        self.setFrameShape(QFrame.Shape.NoFrame)
        self.setStyleSheet("""
            ClipboardItemWidget {
                background: #FFFFFF;
                border: none;
                border-bottom: 1px solid #F0F0F0;
            }
            ClipboardItemWidget:hover {
                background: #F5F5F5;
            }
            QLabel {
                color: #333333;
                background: transparent;
                border: none;
            }
        """)
        
        layout = QVBoxLayout(self)
        layout.setContentsMargins(12, 6, 20, 6)  # 减少上下边距：8→6
        layout.setSpacing(2)  # 减少行间距：4→2
        
        # 第一行：图标/缩略图 + 内容预览
        top_layout = QHBoxLayout()
        
        # 图标或缩略图（只有有内容时才添加）
        if self.item.content_type == "image" and self.item.thumbnail:
            # 显示缩略图
            thumbnail_label = QLabel()
            pixmap = self._load_thumbnail(self.item.thumbnail)
            if pixmap:
                thumbnail_label.setPixmap(pixmap)
                thumbnail_label.setFixedSize(40, 40)
                thumbnail_label.setScaledContents(True)
                thumbnail_label.setStyleSheet("background: transparent; border: none;")
                top_layout.addWidget(thumbnail_label)
        elif self.item.icon:
            # 只有图标不为空时才添加
            icon_label = QLabel(self.item.icon)
            icon_label.setFixedWidth(24)
            icon_label.setStyleSheet("background: transparent; border: none;")
            top_layout.addWidget(icon_label)
        
        # 内容预览（智能自适应高度）
        content_label = QLabel(self.item.display_text)
        font_metrics = content_label.fontMetrics()
        line_height = font_metrics.height()
        
        # 根据 display_lines 设置换行和最大高度
        if self.display_lines > 1:
            # 多行模式：自由换行，但限制最大高度
            content_label.setWordWrap(True)
            # 只设置最大高度，让内容自由伸缩（不设置最小高度）
            max_height = line_height * self.display_lines + self.line_height_padding
            content_label.setMaximumHeight(max_height)
            # 设置文本省略模式（底部截断）
            content_label.setTextFormat(Qt.TextFormat.PlainText)
            # 设置样式，包含行高控制
            content_label.setStyleSheet("""
                font-size: 13px; 
                color: #333333; 
                background: transparent; 
                border: none;
                line-height: 1.2;
            """)
        else:
            # 单行模式：不换行，超长省略
            content_label.setWordWrap(False)
            # 单行时设置固定最小高度
            content_label.setMinimumHeight(line_height + 4)
            content_label.setStyleSheet("font-size: 13px; color: #333333; background: transparent; border: none;")
        
        # 设置文本省略，防止长文本撑开布局
        content_label.setTextInteractionFlags(Qt.TextInteractionFlag.NoTextInteraction)
        from PyQt6.QtWidgets import QSizePolicy
        content_label.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Preferred)
        content_label.setMinimumWidth(0)  # 允许缩小
        top_layout.addWidget(content_label, 1)
        
        # 置顶标记
        if self.item.is_pinned:
            pin_label = QLabel("📌")
            pin_label.setStyleSheet("background: transparent; border: none;")
            top_layout.addWidget(pin_label)
        
        layout.addLayout(top_layout)
        
        # 第二行：来源应用（左） + 时间（右）
        bottom_layout = QHBoxLayout()
        bottom_layout.setContentsMargins(0, 0, 0, 0)
        
        # 左侧：应用名
        if self.item.source_app:
            source_label = QLabel(self.item.source_app)
            source_label.setStyleSheet("color: #999; font-size: 11px; background: transparent; border: none;")
            bottom_layout.addWidget(source_label)
        
        # 中间弹性空间
        bottom_layout.addStretch()
        
        # 右侧：仅显示时间
        if self.item.created_at:
            time_label = QLabel(self.item.created_at.strftime("%H:%M"))
            time_label.setStyleSheet("color: #999; font-size: 11px; background: transparent; border: none;")
            bottom_layout.addWidget(time_label)
        
        layout.addLayout(bottom_layout)
    
    def _load_thumbnail(self, data_url: str) -> Optional[QPixmap]:
        """从 Base64 Data URL 加载缩略图"""
        try:
            import base64
            # 解析 data:image/png;base64,xxxxx 格式
            if data_url.startswith("data:image"):
                # 提取 base64 数据部分
                header, data = data_url.split(",", 1)
                image_data = base64.b64decode(data)
                
                # 创建 QPixmap
                pixmap = QPixmap()
                pixmap.loadFromData(image_data)
                return pixmap
        except Exception as e:
            print(f"❌ [Clipboard] 加载缩略图失败: {e}")
        return None
    
    def mousePressEvent(self, event):
        """鼠标点击"""
        if event.button() == Qt.MouseButton.LeftButton:
            self.clicked.emit(self.item.id)
            # 将双击逻辑移到单击事件中
            self.double_clicked.emit(self.item.id)
        super().mousePressEvent(event)
    
    def mouseDoubleClickEvent(self, event):
        """鼠标双击"""
        # 禁用双击事件的逻辑
        pass
    
    def enterEvent(self, event):
        """鼠标进入 - 触发悬停预览"""
        super().enterEvent(event)
        # 获取鼠标位置，显示预览弹窗
        popup = PreviewPopup.instance()
        pos = QCursor.pos()
        
        # 图片和文件类型显示悬停预览，统一 500ms 延迟
        if self.item.content_type in ("image", "file"):
            popup.show_preview(self.item, pos, delay_ms=500)
    
    def leaveEvent(self, event):
        """鼠标离开 - 隐藏预览"""
        super().leaveEvent(event)
        PreviewPopup.instance().hide_preview()


class ClipboardWindow(QWidget):
    """
    剪贴板历史窗口
    
    显示剪贴板历史记录，支持搜索、筛选、分组等功能。
    """
    
    # 信号
    item_pasted = pyqtSignal(int)  # 粘贴项信号
    closed = pyqtSignal()  # 关闭信号
    new_item_received = pyqtSignal()  # 新内容信号（用于外部触发刷新）
    
    def __init__(self, parent=None):
        super().__init__(parent)
        self.manager = ClipboardManager()
        self.current_items: List[ClipboardItem] = []
        self.selected_item_id: Optional[int] = None
        
        # 记录打开窗口前的活动窗口，用于自动粘贴时恢复焦点
        self._previous_window_hwnd = None
        
        # 分组相关
        self.current_group_id: Optional[int] = None  # None 表示显示剪切板历史
        self.group_buttons: List[QPushButton] = []
        
        # 分页加载相关
        self._current_offset = 0  # 当前加载的偏移量
        self._page_size = 50  # 每页加载数量
        self._is_loading = False  # 是否正在加载
        self._has_more = True  # 是否还有更多数据
        self._last_scroll_value = 0  # 上次滚动位置，用于判断滚动方向
        
        # 连接新内容信号到刷新方法
        self.new_item_received.connect(self._on_new_item)
        
        # 窗口拖动相关
        self._drag_pos: Optional[QPoint] = None
        self._is_dragging = False
        
        # 窗口调整大小相关
        self._resize_edge = None  # 'left', 'right', 'top', 'bottom', 'topleft', 'topright', 'bottomleft', 'bottomright'
        self._resize_start_pos = None
        self._resize_start_geometry = None
        self._edge_margin = 8  # 边缘检测范围
        
        # 使用 QSettings 保存窗口位置和大小
        self._qsettings = QSettings("Jietuba", "ClipboardWindow")
        
        # 加载设置
        self._load_settings()
        
        # 加载窗口位置和大小
        self._load_window_geometry()
        
        self._setup_ui()
        self._setup_shortcuts()
        self._load_history()
        
        # 启用鼠标追踪以检测边缘
        self.setMouseTracking(True)
        
        # 安装事件过滤器用于检测边缘光标
        self.installEventFilter(self)
        
        # 设置预览弹窗的管理器
        PreviewPopup.instance().set_manager(self.manager)
    
    def _load_settings(self):
        """加载设置"""
        try:
            from settings import get_tool_settings_manager
            config = get_tool_settings_manager()
            self.auto_paste_enabled = config.get_clipboard_auto_paste()
            self.paste_with_html = config.get_app_setting("clipboard_paste_with_html", True)
            # 透明度设置：0-100，100 表示完全透明（使用专用的 getter 方法）
            self.window_opacity = config.get_clipboard_window_opacity()
            # 显示行数设置：1, 2, 3（使用专用的 getter 方法）
            self.display_lines = config.get_clipboard_display_lines()
            self._apply_opacity()
        except Exception:
            # 默认开启自动粘贴和带格式粘贴
            self.auto_paste_enabled = True
            self.paste_with_html = True
            self.window_opacity = 0
            self.display_lines = 1
    
    def _apply_opacity(self):
        """应用窗口透明度"""
        # window_opacity: 0=不透明, 100=完全透明
        # Qt opacity: 1.0=不透明, 0.0=完全透明
        opacity = 1.0 - (self.window_opacity / 100.0)
        self.setWindowOpacity(opacity)
    
    def _load_window_geometry(self):
        """加载窗口位置和大小"""
        try:
            # 从 tool_settings 获取默认值
            from settings import get_tool_settings_manager
            config = get_tool_settings_manager()
            default_width = config.get_app_setting("clipboard_window_width", 450)
            default_height = config.get_app_setting("clipboard_window_height", 600)
        except Exception:
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
            Qt.WindowType.WindowStaysOnTopHint
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
        content_layout = QHBoxLayout(self.container)
        content_layout.setContentsMargins(0, 0, 0, 0)
        content_layout.setSpacing(0)
        
        # ========== 左侧主内容区 ==========
        left_widget = QWidget()
        left_widget.setStyleSheet("background: transparent;")
        left_layout = QVBoxLayout(left_widget)
        left_layout.setContentsMargins(0, 0, 0, 0)
        left_layout.setSpacing(0)
        
        # 历史列表
        self.list_widget = QListWidget()
        self.list_widget.setStyleSheet("""
            QListWidget {
                background: #FFFFFF;
                border: none;
                outline: none;
            }
            QListWidget::item {
                padding: 2px 4px;
                border: none;
                background: transparent;
            }
            QListWidget::item:selected {
                background: transparent;
            }
            QListWidget::item:hover {
                background: transparent;
            }
        """)
        self.list_widget.setSpacing(2)
        self.list_widget.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.list_widget.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAsNeeded)
        self.list_widget.itemDoubleClicked.connect(self._on_item_double_clicked)
        self.list_widget.itemClicked.connect(self._on_item_clicked)
        self.list_widget.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        self.list_widget.customContextMenuRequested.connect(self._show_context_menu)
        
        # 连接滚动条信号，实现滚动加载
        scrollbar = self.list_widget.verticalScrollBar()
        scrollbar.valueChanged.connect(self._on_scroll)
        
        left_layout.addWidget(self.list_widget, 1)
        
        # 底部搜索栏
        bottom_bar = QWidget()
        bottom_bar.setFixedHeight(36)
        bottom_bar.setStyleSheet("""
            QWidget {
                background: #FAFAFA;
                border-top: 1px solid #E0E0E0;
            }
        """)
        bottom_layout = QHBoxLayout(bottom_bar)
        bottom_layout.setContentsMargins(8, 4, 8, 4)
        bottom_layout.setSpacing(8)
        
        # 搜索图标
        search_icon = QLabel("🔍")
        search_icon.setStyleSheet("background: transparent; border: none;")
        bottom_layout.addWidget(search_icon)
        
        # 搜索输入框
        self.search_input = QLineEdit()
        self.search_input.setPlaceholderText(self.tr("Search"))
        self.search_input.setStyleSheet("""
            QLineEdit {
                background: transparent;
                border: none;
                color: #333333;
                font-size: 13px;
                padding: 4px;
            }
        """)
        self.search_input.textChanged.connect(self._on_search_changed)
        bottom_layout.addWidget(self.search_input, 1)
        
        # 隐藏的类型筛选（保留功能但不显示）
        self.type_filter = QComboBox()
        self.type_filter.addItems([self.tr("All"), self.tr("Text"), self.tr("Image"), self.tr("File")])
        self.type_filter.hide()
        
        # 三点菜单按钮
        self.menu_btn = QPushButton("···")
        self.menu_btn.setFixedSize(28, 28)
        self.menu_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        self.menu_btn.setStyleSheet("""
            QPushButton {
                background: transparent;
                color: #666666;
                border: none;
                font-size: 16px;
                font-weight: bold;
            }
            QPushButton:hover {
                background: #E0E0E0;
                border-radius: 4px;
            }
        """)
        self.menu_btn.clicked.connect(self._show_main_menu)
        bottom_layout.addWidget(self.menu_btn)
        
        left_layout.addWidget(bottom_bar)
        
        content_layout.addWidget(left_widget, 1)
        
        # ========== 右侧按钮栏 ==========
        self.right_bar = QWidget()
        self.right_bar.setFixedWidth(40)
        self.right_bar.setStyleSheet("""
            QWidget {
                background: #FAFAFA;
                border-left: 1px solid #E0E0E0;
            }
        """)
        right_layout = QVBoxLayout(self.right_bar)
        right_layout.setContentsMargins(2, 8, 2, 8)
        right_layout.setSpacing(4)
        
        # 关闭按钮
        close_btn = QPushButton("×")
        close_btn.setFixedSize(34, 34)
        close_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        close_btn.setToolTip(self.tr("Close"))
        close_btn.setStyleSheet("""
            QPushButton {
                background: transparent;
                color: #999999;
                border: none;
                font-size: 20px;
                padding: 0px;
            }
            QPushButton:hover {
                background: #FFEBEE;
                color: #F44336;
                border-radius: 4px;
            }
        """)
        close_btn.clicked.connect(self.close)
        right_layout.addWidget(close_btn)
        
        # 分隔线
        separator1 = QFrame()
        separator1.setFixedHeight(1)
        separator1.setStyleSheet("background: #E0E0E0;")
        right_layout.addWidget(separator1)
        
        # 剪切板按钮（显示所有历史）
        self.clipboard_btn = QPushButton("📋")
        self.clipboard_btn.setFixedSize(34, 34)
        self.clipboard_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        self.clipboard_btn.setToolTip(self.tr("Clipboard History"))
        self.clipboard_btn.setCheckable(True)
        self.clipboard_btn.setChecked(True)
        self.clipboard_btn.setStyleSheet(self._get_sidebar_btn_style())
        self.clipboard_btn.clicked.connect(lambda: self._switch_to_group(None))
        right_layout.addWidget(self.clipboard_btn)
        
        # 分组按钮容器
        self.group_buttons_widget = QWidget()
        self.group_buttons_widget.setStyleSheet("background: transparent;")
        self.group_buttons_layout = QVBoxLayout(self.group_buttons_widget)
        self.group_buttons_layout.setContentsMargins(0, 0, 0, 0)
        self.group_buttons_layout.setSpacing(4)
        right_layout.addWidget(self.group_buttons_widget)
        
        right_layout.addStretch()
        
        # 分隔线
        separator2 = QFrame()
        separator2.setFixedHeight(1)
        separator2.setStyleSheet("background: #E0E0E0;")
        right_layout.addWidget(separator2)
        
        # 添加分组按钮
        self.add_group_btn = QPushButton("+")
        self.add_group_btn.setFixedSize(34, 34)
        self.add_group_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        self.add_group_btn.setToolTip(self.tr("Add Group"))
        self.add_group_btn.setStyleSheet("""
            QPushButton {
                background: transparent;
                color: #4CAF50;
                border: 1px dashed #4CAF50;
                border-radius: 4px;
                font-size: 20px;
                font-weight: bold;
                padding: 0px;
            }
            QPushButton:hover {
                background: #E8F5E9;
            }
        """)
        self.add_group_btn.clicked.connect(self._on_add_group_clicked)
        right_layout.addWidget(self.add_group_btn)
        
        content_layout.addWidget(self.right_bar)
        
        # 初始化分组按钮
        self._refresh_group_buttons()
        
        # 为所有子控件启用鼠标追踪和事件过滤器，以便检测边缘
        self._setup_mouse_tracking_recursive(self)
    
    def _get_sidebar_btn_style(self):
        """获取侧边栏按钮样式"""
        return """
            QPushButton {
                background: transparent;
                color: #666666;
                border: none;
                font-size: 20px;
                border-radius: 4px;
                padding: 0px;
            }
            QPushButton:hover {
                background: #E0E0E0;
            }
            QPushButton:checked {
                background: #E3F2FD;
                color: #1976D2;
            }
            QToolTip {
                background: #FFFFFF;
                color: #333333;
                border: 1px solid #E0E0E0;
                border-radius: 4px;
                padding: 4px 8px;
                font-size: 12px;
            }
        """
    
    def _show_main_menu(self):
        """显示主菜单（三点按钮）"""
        menu = QMenu(self)
        menu.setStyleSheet("""
            QMenu {
                background: #FFFFFF;
                border: 1px solid #E0E0E0;
                border-radius: 4px;
                padding: 4px;
            }
            QMenu::item {
                padding: 8px 20px;
                color: #333333;
            }
            QMenu::item:selected {
                background: #F5F5F5;
            }
            QMenu::separator {
                height: 1px;
                background: #E0E0E0;
                margin: 4px 8px;
            }
        """)
        
        # 筛选子菜单
        filter_menu = menu.addMenu(self.tr("Filter Type"))
        filter_actions = []
        for i, name in enumerate([self.tr("All"), self.tr("Text"), self.tr("Image"), self.tr("File")]):
            action = filter_menu.addAction(name)
            action.setCheckable(True)
            action.setChecked(self.type_filter.currentIndex() == i)
            action.triggered.connect(lambda checked, idx=i: self._set_filter(idx))
            filter_actions.append(action)
        
        menu.addSeparator()
        
        # 粘贴带格式开关
        paste_html_action = menu.addAction(self.tr("📋 Paste with Format"))
        paste_html_action.setCheckable(True)
        paste_html_action.setChecked(self.paste_with_html)
        paste_html_action.triggered.connect(self._toggle_paste_with_html)
        
        # 自动粘贴开关
        try:
            from settings import get_tool_settings_manager
            config = get_tool_settings_manager()
            auto_paste = config.get_clipboard_auto_paste()
        except Exception:
            auto_paste = True
        
        auto_paste_action = menu.addAction(self.tr("🔄 Auto Paste After Selection"))
        auto_paste_action.setCheckable(True)
        auto_paste_action.setChecked(auto_paste)
        auto_paste_action.triggered.connect(self._toggle_auto_paste)
        
        # 粘贴后移到最前开关
        try:
            from settings import get_tool_settings_manager
            config = get_tool_settings_manager()
            move_to_top = config.get_clipboard_move_to_top_on_paste()
        except Exception:
            move_to_top = True
        
        move_to_top_action = menu.addAction(self.tr("⬆️ Move to Top After Paste"))
        move_to_top_action.setCheckable(True)
        move_to_top_action.setChecked(move_to_top)
        move_to_top_action.triggered.connect(self._toggle_move_to_top_on_paste)
        
        menu.addSeparator()
        
        # 透明度子菜单（从配置中读取选项）
        opacity_menu = menu.addMenu(self.tr("🔲 Window Opacity"))
        try:
            from settings import get_tool_settings_manager
            config = get_tool_settings_manager()
            opacity_options = config.get_clipboard_window_opacity_options()
        except Exception:
            opacity_options = [0, 5, 10, 15, 20, 25]  # 备用默认值
        
        for percent in opacity_options:
            if percent == 0:
                label = self.tr("Opaque")
            else:
                label = f"{percent}%"
            action = opacity_menu.addAction(label)
            action.setCheckable(True)
            action.setChecked(self.window_opacity == percent)
            action.triggered.connect(lambda checked, p=percent: self._set_window_opacity(p))
        
        # 显示行数子菜单（说明是最大行数）
        lines_menu = menu.addMenu(self.tr("📏 Max Display Lines"))
        for lines in [1, 2]:
            action = lines_menu.addAction(self.tr("%n line(s)", "", lines))
            action.setCheckable(True)
            action.setChecked(self.display_lines == lines)
            action.triggered.connect(lambda checked, n=lines: self._set_display_lines(n))
        
        menu.addSeparator()
        
        # 添加内容
        add_action = menu.addAction(self.tr("📝 Add Content"))
        add_action.triggered.connect(self._on_add_item_clicked)
        
        # 在按钮上方显示菜单
        pos = self.menu_btn.mapToGlobal(QPoint(0, -menu.sizeHint().height()))
        menu.exec(pos)
    
    def _set_filter(self, index: int):
        """设置筛选类型"""
        self.type_filter.setCurrentIndex(index)
        self._load_history()
    
    def _toggle_paste_with_html(self, checked: bool):
        """切换粘贴带格式设置"""
        self.paste_with_html = checked
        try:
            from settings import get_tool_settings_manager
            config = get_tool_settings_manager()
            config.set_app_setting("clipboard_paste_with_html", checked)
        except Exception:
            pass
    
    def _toggle_auto_paste(self, checked: bool):
        """切换自动粘贴设置"""
        try:
            from settings import get_tool_settings_manager
            config = get_tool_settings_manager()
            config.set_clipboard_auto_paste(checked)
        except Exception:
            pass
    
    def _toggle_move_to_top_on_paste(self, checked: bool):
        """切换粘贴后移到最前设置"""
        try:
            from settings import get_tool_settings_manager
            config = get_tool_settings_manager()
            config.set_clipboard_move_to_top_on_paste(checked)
        except Exception:
            pass
    
    def _set_window_opacity(self, percent: int):
        """设置窗口透明度"""
        self.window_opacity = percent
        self._apply_opacity()
        # 保存到设置（使用专用的 setter 方法）
        try:
            from settings import get_tool_settings_manager
            config = get_tool_settings_manager()
            config.set_clipboard_window_opacity(percent)
        except Exception:
            pass
    
    def _set_display_lines(self, lines: int):
        """设置显示行数"""
        self.display_lines = lines
        # 保存到设置（使用专用的 setter 方法）
        try:
            from settings import get_tool_settings_manager
            config = get_tool_settings_manager()
            config.set_clipboard_display_lines(lines)
        except Exception:
            pass
        # 刷新列表显示
        self._refresh_list()
    
    def _setup_shortcuts(self):
        """设置快捷键"""
        # Escape 关闭
        QShortcut(QKeySequence(Qt.Key.Key_Escape), self, self.close)
        
        # Enter 粘贴选中项
        QShortcut(QKeySequence(Qt.Key.Key_Return), self, self._paste_selected)
        
        # Delete 删除选中项
        QShortcut(QKeySequence(Qt.Key.Key_Delete), self, self._delete_selected)
        
        # Ctrl+F 聚焦搜索框
        QShortcut(QKeySequence("Ctrl+F"), self, lambda: self.search_input.setFocus())
    
    def _load_history(self):
        """加载历史记录（重置并加载第一页）"""
        # 重置分页状态
        self._current_offset = 0
        self._has_more = True
        self.current_items = []
        
        # 加载第一页
        self._load_more_items()
    
    def _load_more_items(self):
        """加载更多项目（分页加载）"""
        if self._is_loading or not self._has_more:
            return
        
        self._is_loading = True
        t_total_start = perf_counter()
        print(f"📥 [Clipboard] 开始加载更多 - offset: {self._current_offset}, page_size: {self._page_size}")
        
        try:
            search = self.search_input.text().strip() or None
            
            # 获取类型筛选
            type_map = {0: None, 1: "text", 2: "image", 3: "file"}
            content_type = type_map.get(self.type_filter.currentIndex())
            
            # 根据当前分组加载内容
            t_query_start = perf_counter()
            if self.current_group_id is None:
                # 显示剪切板历史
                new_items = self.manager.get_history(
                    limit=self._page_size,
                    offset=self._current_offset,
                    search=search,
                    content_type=content_type
                )
            else:
                # 显示分组内容
                new_items = self.manager.get_by_group(
                    group_id=self.current_group_id,
                    limit=self._page_size,
                    offset=self._current_offset
                )
                # 如果有搜索词，过滤分组内容
                if search:
                    search_lower = search.lower()
                    new_items = [
                        item for item in new_items 
                        if search_lower in item.content.lower()
                    ]
            t_query_end = perf_counter()
            print(f"⏱️ [Clipboard] 查询耗时: {(t_query_end - t_query_start) * 1000:.1f} ms，获取 {len(new_items)} 条")
            
            print(f"✅ [Clipboard] 加载完成 - 获取到 {len(new_items)} 条记录")
            
            # 检查是否还有更多数据
            if len(new_items) < self._page_size:
                self._has_more = False
                print(f"⏹️ [Clipboard] 没有更多数据了")
            
            # 追加到当前列表
            if new_items:
                self.current_items.extend(new_items)
                self._current_offset += len(new_items)
                
                # 如果是第一页，清空列表；否则追加
                if self._current_offset == len(new_items):
                    self._refresh_list()
                else:
                    self._append_items(new_items)
                    
                print(f"📊 [Clipboard] 当前总计: {len(self.current_items)} 条记录")
        
        finally:
            t_total_end = perf_counter()
            print(f"⏱️ [Clipboard] 本批次总耗时: {(t_total_end - t_total_start) * 1000:.1f} ms")
            self._is_loading = False
    
    def _refresh_list(self):
        """刷新列表显示"""
        self.list_widget.clear()
        
        # 强制处理事件，确保布局更新
        QApplication.processEvents()
        
        # 获取列表视口宽度，确保 widget 有正确的宽度
        viewport_width = self.list_widget.viewport().width()
        if viewport_width < 100:
            viewport_width = self.list_widget.width() - 20  # 减去滚动条宽度
        if viewport_width < 100:
            viewport_width = 300  # 默认值
        
        for item in self.current_items:
            widget = ClipboardItemWidget(item, display_lines=self.display_lines)
            widget.setFixedWidth(viewport_width)  # 设置固定宽度确保布局正确
            widget.double_clicked.connect(self._on_paste_item)
            
            list_item = QListWidgetItem()
            # 设置合适的尺寸
            list_item.setSizeHint(QSize(viewport_width, widget.sizeHint().height()))
            list_item.setData(Qt.ItemDataRole.UserRole, item.id)
            
            self.list_widget.addItem(list_item)
            self.list_widget.setItemWidget(list_item, widget)
        
        # 刷新完成后再次处理事件确保显示正确
        self.list_widget.update()
    
    def _append_items(self, items: List[ClipboardItem]):
        """追加项目到列表末尾（用于分页加载）"""
        viewport_width = self.list_widget.viewport().width()
        if viewport_width < 100:
            viewport_width = self.list_widget.width() - 20
        if viewport_width < 100:
            viewport_width = 300
        
        for item in items:
            widget = ClipboardItemWidget(item, display_lines=self.display_lines)
            widget.setFixedWidth(viewport_width)
            widget.double_clicked.connect(self._on_paste_item)
            
            list_item = QListWidgetItem()
            list_item.setSizeHint(QSize(viewport_width, widget.sizeHint().height()))
            list_item.setData(Qt.ItemDataRole.UserRole, item.id)
            
            self.list_widget.addItem(list_item)
            self.list_widget.setItemWidget(list_item, widget)
        
        self.list_widget.update()
    
    def _on_scroll(self, value: int):
        """滚动条值变化时检查是否需要加载更多"""
        if not self._has_more or self._is_loading:
            return
        
        scrollbar = self.list_widget.verticalScrollBar()
        maximum = scrollbar.maximum()
        
        # 如果没有滚动条（maximum <= 0），不触发加载
        if maximum <= 0:
            return
        
        # 只在向下滚动时触发加载
        if value <= self._last_scroll_value:
            self._last_scroll_value = value
            return
        
        self._last_scroll_value = value
        
        # 计算距离底部的距离
        distance_to_bottom = maximum - value
        
        # 计算当前滚动位置的百分比
        scroll_percentage = (value / maximum * 100) if maximum > 0 else 0
        
        # 必须满足两个条件才触发加载：
        # 1. 距离底部小于 50 像素
        # 2. 滚动位置超过 90%
        if distance_to_bottom < 50 and scroll_percentage > 90:
            print(f"🔄 [Clipboard] 触发加载更多 - 距离底部: {distance_to_bottom}px, 滚动位置: {scroll_percentage:.1f}%")
            self._load_more_items()
    
    def _on_search_changed(self, text: str):
        """搜索文本变化"""
        # 延迟搜索，避免频繁查询
        if hasattr(self, '_search_timer'):
            self._search_timer.stop()
        else:
            self._search_timer = QTimer()
            self._search_timer.setSingleShot(True)
            self._search_timer.timeout.connect(self._load_history)
        
        self._search_timer.start(300)
    
    def _on_filter_changed(self, index: int):
        """类型筛选变化"""
        self._load_history()
    
    # ==================== 分组功能 ====================
    
    def _refresh_group_buttons(self):
        """刷新分组按钮"""
        # 清除现有按钮
        for btn in self.group_buttons:
            btn.deleteLater()
        self.group_buttons.clear()
        
        # 更新剪切板按钮状态
        self.clipboard_btn.setChecked(self.current_group_id is None)
        
        # 添加各个分组按钮
        groups = self.manager.get_groups()
        for group in groups:
            icon = group.icon if group.icon else "📁"
            btn = QPushButton(icon)
            btn.setFixedSize(34, 34)
            btn.setCursor(Qt.CursorShape.PointingHandCursor)
            btn.setToolTip(group.name)
            btn.setCheckable(True)
            btn.setChecked(self.current_group_id == group.id)
            btn.setStyleSheet(self._get_sidebar_btn_style())
            btn.setProperty("group_id", group.id)
            btn.clicked.connect(lambda checked, gid=group.id: self._switch_to_group(gid))
            # 右键菜单删除分组
            btn.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
            btn.customContextMenuRequested.connect(
                lambda pos, b=btn, gid=group.id: self._show_group_context_menu(b, gid, pos)
            )
            self.group_buttons_layout.addWidget(btn)
            self.group_buttons.append(btn)
    
    def _get_group_btn_style(self):
        """获取分组按钮样式（旧版，保留兼容）"""
        return self._get_sidebar_btn_style()
    
    def _switch_to_group(self, group_id: Optional[int]):
        """切换到指定分组"""
        self.current_group_id = group_id
        
        # 更新剪切板按钮状态
        self.clipboard_btn.setChecked(group_id is None)
        
        # 更新分组按钮选中状态
        for btn in self.group_buttons:
            btn_group_id = btn.property("group_id")
            btn.setChecked(btn_group_id == group_id)
        
        # 重新加载内容
        self._load_history()
    
    def _show_group_context_menu(self, btn, group_id: int, pos):
        """显示分组右键菜单"""
        menu = QMenu(self)
        menu.setStyleSheet("""
            QMenu {
                background: #FFFFFF;
                border: 1px solid #E5E5E5;
                border-radius: 6px;
                padding: 4px;
            }
            QMenu::item {
                padding: 6px 20px;
                color: #333333;
                border-radius: 4px;
            }
            QMenu::item:selected {
                background: #E8F5E9;
            }
        """)
        
        delete_action = menu.addAction(self.tr("🗑️ Delete Group"))
        delete_action.triggered.connect(lambda: self._delete_group(group_id))
        
        menu.exec(btn.mapToGlobal(pos))
    
    def _delete_group(self, group_id: int):
        """删除分组"""
        reply = QMessageBox.question(
            self, self.tr("Confirm Delete"),
            self.tr("Are you sure you want to delete this group?\nAll items in the group will also be deleted."),
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
            QMessageBox.StandardButton.No
        )
        
        if reply == QMessageBox.StandardButton.Yes:
            if self.manager.delete_group(group_id):
                # 如果当前正在显示被删除的分组，切换到剪切板
                if self.current_group_id == group_id:
                    self.current_group_id = None
                self._refresh_group_buttons()
                self._load_history()
    
    def _on_add_group_clicked(self):
        """点击添加按钮 - 打开管理对话框"""
        dialog = ManageDialog(self.manager, self)
        dialog.group_added.connect(self._refresh_group_buttons)
        dialog.content_added.connect(lambda gid: self._load_history())
        dialog.exec()
    
    def _on_add_item_clicked(self):
        """点击添加内容按钮 - 打开管理对话框并切换到内容页"""
        dialog = ManageDialog(self.manager, self)
        dialog.group_added.connect(self._refresh_group_buttons)
        dialog.content_added.connect(lambda gid: self._load_history())
        dialog._switch_page(1)  # 切换到添加内容页
        dialog.exec()
    
    # ==================== 列表操作 ====================

    def _on_item_clicked(self, item: QListWidgetItem):
        """列表项单击 - 直接粘贴（类似 Ditto 行为）"""
        item_id = item.data(Qt.ItemDataRole.UserRole)
        if item_id:
            self._on_paste_item(item_id)

    def _on_item_double_clicked(self, item: QListWidgetItem):
        """列表项双击"""
        item_id = item.data(Qt.ItemDataRole.UserRole)
        if item_id:
            self._on_paste_item(item_id)
    
    def _on_paste_item(self, item_id: int):
        """粘贴项"""
        # 读取"粘贴后移到最前"设置
        from settings import get_tool_settings_manager
        config = get_tool_settings_manager()
        move_to_top = config.get_clipboard_move_to_top_on_paste()
        
        # 🔑 关键：只在"剪贴板历史"视图时才移动到最前
        # 如果在"收藏分组"视图，则不移动顺序
        if self.current_group_id is not None:
            move_to_top = False  # 在分组中粘贴，不移动顺序
        
        if self.manager.paste_item(item_id, self.paste_with_html, move_to_top):
            print(f"✅ [Clipboard] 已粘贴项 {item_id} (带格式: {self.paste_with_html}, 移到最前: {move_to_top})")
            self.item_pasted.emit(item_id)
            
            # 粘贴后关闭窗口
            self.close()
            
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
    
    def _paste_selected(self):
        """粘贴选中项"""
        current = self.list_widget.currentItem()
        if current:
            item_id = current.data(Qt.ItemDataRole.UserRole)
            if item_id:
                self._on_paste_item(item_id)
    
    def _delete_selected(self):
        """删除选中项"""
        current = self.list_widget.currentItem()
        if current:
            item_id = current.data(Qt.ItemDataRole.UserRole)
            if item_id and self.manager.delete_item(item_id):
                self._load_history()
    
    def _show_context_menu(self, pos):
        """显示右键菜单"""
        item = self.list_widget.itemAt(pos)
        if not item:
            return
        
        item_id = item.data(Qt.ItemDataRole.UserRole)
        if not item_id:
            return
        
        menu = QMenu(self)
        menu.setStyleSheet("""
            QMenu {
                background: #FFFFFF;
                border: 1px solid #E5E5E5;
                border-radius: 6px;
                padding: 4px;
            }
            QMenu::item {
                padding: 6px 20px;
                color: #333333;
                border-radius: 4px;
            }
            QMenu::item:selected {
                background: #E8F5E9;
            }
            QMenu::separator {
                height: 1px;
                background: #E5E5E5;
                margin: 4px 10px;
            }
        """)
        
        # 粘贴
        paste_action = menu.addAction(self.tr("📋 Paste"))
        paste_action.triggered.connect(lambda: self._on_paste_item(item_id))
        
        menu.addSeparator()
        
        # 置顶
        clipboard_item = self.manager.get_item(item_id)
        if clipboard_item:
            pin_text = self.tr("Unpin") if clipboard_item.is_pinned else self.tr("📌 Pin")
            pin_action = menu.addAction(pin_text)
            pin_action.triggered.connect(lambda: self._toggle_pin(item_id))
        
        menu.addSeparator()
        
        # 移动到分组子菜单
        groups = self.manager.get_groups()
        if groups:
            move_menu = menu.addMenu(self.tr("📁 Move to Group"))
            move_menu.setStyleSheet("""
                QMenu {
                    background: #FFFFFF;
                    border: 1px solid #E5E5E5;
                    border-radius: 6px;
                    padding: 4px;
                }
                QMenu::item {
                    padding: 6px 20px;
                    color: #333333;
                    border-radius: 4px;
                }
                QMenu::item:selected {
                    background: #E8F5E9;
                }
            """)
            
            for group in groups:
                display_text = f"{group.icon} {group.name}" if group.icon else group.name
                action = move_menu.addAction(display_text)
                action.triggered.connect(
                    lambda checked, gid=group.id: self._move_item_to_group(item_id, gid)
                )
            
            # 从分组移出（移回剪切板历史）
            if self.current_group_id is not None:
                move_menu.addSeparator()
                remove_action = move_menu.addAction(self.tr("↩️ Remove from Group"))
                remove_action.triggered.connect(
                    lambda: self._move_item_to_group(item_id, None)
                )
        
        menu.addSeparator()
        
        # 删除
        delete_action = menu.addAction(self.tr("🗑️ Delete"))
        delete_action.triggered.connect(lambda: self._delete_item(item_id))
        
        menu.exec(self.list_widget.mapToGlobal(pos))
    
    def _move_item_to_group(self, item_id: int, group_id: Optional[int]):
        """将项目移动到分组"""
        if self.manager.move_to_group(item_id, group_id):
            print(f"✅ [Clipboard] 已移动到分组 {group_id}")
            self._load_history()
    
    def _toggle_pin(self, item_id: int):
        """切换置顶"""
        self.manager.toggle_pin(item_id)
        self._load_history()
    
    def _delete_item(self, item_id: int):
        """删除项"""
        if self.manager.delete_item(item_id):
            self._load_history()
    
    def _on_clear_clicked(self):
        """清空历史"""
        reply = QMessageBox.question(
            self, self.tr("Confirm Clear"),
            self.tr("Are you sure you want to clear all clipboard history?\nThis action cannot be undone."),
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
            QMessageBox.StandardButton.No
        )
        
        if reply == QMessageBox.StandardButton.Yes:
            if self.manager.clear_history():
                self._load_history()
    
    def _on_new_item(self):
        """收到新剪贴板内容时刷新列表"""
        # 只在窗口可见时刷新
        if self.isVisible():
            self._load_history()
    
    def notify_new_content(self):
        """外部调用：通知有新内容（线程安全）"""
        # 使用信号确保在主线程中执行
        self.new_item_received.emit()
    
    def showEvent(self, event):
        """显示时刷新"""
        # 记录当前前台窗口（在显示剪贴板窗口之前）
        self._previous_window_hwnd = get_foreground_window()
        
        t_show_start = perf_counter()
        super().showEvent(event)
        self._load_history()
        self.search_input.setFocus()
        
        # 每次显示都定位到鼠标位置（右下方）
        self._position_at_cursor()
        t_show_end = perf_counter()
        print(f"⏱️ [Clipboard] 打开窗口耗时: {(t_show_end - t_show_start) * 1000:.1f} ms")
    
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
    
    def hideEvent(self, event):
        """隐藏时保存位置和大小，并关闭预览窗口"""
        super().hideEvent(event)
        self._save_window_geometry()
        # 关闭预览弹窗
        PreviewPopup.instance().hide_preview()
        # 关闭所有活动的菜单/弹窗
        active_popup = QApplication.activePopupWidget()
        if active_popup is not None:
            active_popup.close()
    
    def closeEvent(self, event):
        """关闭事件"""
        self._save_window_geometry()
        # 关闭预览弹窗
        PreviewPopup.instance().hide_preview()
        # 关闭所有活动的菜单/弹窗
        active_popup = QApplication.activePopupWidget()
        if active_popup is not None:
            active_popup.close()
        self.closed.emit()
        super().closeEvent(event)
    
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
    
    def _setup_mouse_tracking_recursive(self, widget):
        """递归为所有子控件启用鼠标追踪和安装事件过滤器"""
        widget.setMouseTracking(True)
        widget.installEventFilter(self)
        for child in widget.findChildren(QWidget):
            child.setMouseTracking(True)
            child.installEventFilter(self)
    
    def _get_edge_at_pos(self, pos: QPoint) -> str:
        """获取鼠标位置对应的边缘，返回边缘名称或空字符串"""
        x, y = pos.x(), pos.y()
        w, h = self.width(), self.height()
        m = self._edge_margin
        
        on_left = x < m
        on_right = x > w - m
        on_top = y < m
        on_bottom = y > h - m
        
        if on_top and on_left:
            return 'topleft'
        if on_top and on_right:
            return 'topright'
        if on_bottom and on_left:
            return 'bottomleft'
        if on_bottom and on_right:
            return 'bottomright'
        if on_left:
            return 'left'
        if on_right:
            return 'right'
        if on_top:
            return 'top'
        if on_bottom:
            return 'bottom'
        return ''
    
    def _is_draggable_area(self, widget, local_pos: QPoint) -> bool:
        """检查是否在可拖动区域（仅右侧边栏空白处）"""
        # 右侧边栏空白区域可拖动（非按钮区域）
        if hasattr(self, 'right_bar') and widget:
            # 检查 widget 是否是 right_bar 本身（而非其子按钮）
            right_bar = getattr(self, 'right_bar', None)
            if right_bar and (widget is right_bar or widget.parent() is right_bar):
                # 如果点击的是 right_bar 本身（空白处），允许拖动
                if widget is right_bar:
                    return True
        return False
    
    def _update_cursor_shape(self, edge: str):
        """根据边缘更新鼠标光标形状"""
        cursor_map = {
            'left': Qt.CursorShape.SizeHorCursor,
            'right': Qt.CursorShape.SizeHorCursor,
            'top': Qt.CursorShape.SizeVerCursor,
            'bottom': Qt.CursorShape.SizeVerCursor,
            'topleft': Qt.CursorShape.SizeFDiagCursor,
            'bottomright': Qt.CursorShape.SizeFDiagCursor,
            'topright': Qt.CursorShape.SizeBDiagCursor,
            'bottomleft': Qt.CursorShape.SizeBDiagCursor,
        }
        if edge in cursor_map:
            self.setCursor(cursor_map[edge])
        else:
            self.unsetCursor()
    
    def _do_resize(self, global_pos: QPoint):
        """执行窗口调整大小"""
        if not self._resize_start_geometry or not self._resize_start_pos:
            return
        
        dx = global_pos.x() - self._resize_start_pos.x()
        dy = global_pos.y() - self._resize_start_pos.y()
        
        geo = self._resize_start_geometry
        new_x, new_y = geo.x(), geo.y()
        new_w, new_h = geo.width(), geo.height()
        min_w, min_h = self.minimumWidth(), self.minimumHeight()
        
        edge = self._resize_edge
        
        if 'left' in edge:
            new_w = max(min_w, geo.width() - dx)
            if new_w > min_w:
                new_x = geo.x() + dx
        if 'right' in edge:
            new_w = max(min_w, geo.width() + dx)
        if 'top' in edge:
            new_h = max(min_h, geo.height() - dy)
            if new_h > min_h:
                new_y = geo.y() + dy
        if 'bottom' in edge:
            new_h = max(min_h, geo.height() + dy)
        
        self.setGeometry(new_x, new_y, new_w, new_h)
    
    def _reset_drag_state(self):
        """重置拖动状态"""
        self._is_dragging = False
        self._drag_pos = None
        self._resize_edge = None
        self._resize_start_pos = None
        self._resize_start_geometry = None
        self.unsetCursor()
    
    def eventFilter(self, obj, event):
        """统一处理拖动和调整大小事件"""
        from PyQt6.QtCore import QEvent
        
        event_type = event.type()
        
        # 获取鼠标位置（如果可用）
        global_pos = None
        local_pos = None
        if hasattr(event, 'globalPosition'):
            global_pos = event.globalPosition().toPoint()
            local_pos = self.mapFromGlobal(global_pos)
        
        # 鼠标移动
        if event_type == QEvent.Type.MouseMove and local_pos:
            if self._resize_edge:
                self._do_resize(global_pos)
                return True
            if self._is_dragging and self._drag_pos:
                self.move(global_pos - self._drag_pos)
                return True
            # 更新光标
            edge = self._get_edge_at_pos(local_pos)
            self._update_cursor_shape(edge)
        
        # 鼠标按下
        elif event_type == QEvent.Type.MouseButtonPress and local_pos:
            if event.button() == Qt.MouseButton.LeftButton:
                edge = self._get_edge_at_pos(local_pos)
                if edge:
                    # 开始调整大小
                    self._resize_edge = edge
                    self._resize_start_pos = global_pos
                    self._resize_start_geometry = self.geometry()
                    return True
                elif self._is_draggable_area(obj, local_pos):
                    # 开始拖动窗口
                    self._is_dragging = True
                    self._drag_pos = global_pos - self.pos()
                    return True
        
        # 鼠标释放
        elif event_type == QEvent.Type.MouseButtonRelease:
            if event.button() == Qt.MouseButton.LeftButton:
                if self._resize_edge or self._is_dragging:
                    self._reset_drag_state()
                    return True
        
        return super().eventFilter(obj, event)
    
    def leaveEvent(self, event):
        """鼠标离开窗口时重置光标"""
        if not self._is_dragging and not self._resize_edge:
            self.unsetCursor()
        super().leaveEvent(event)
    
    def keyPressEvent(self, event):
        """键盘事件"""
        # 数字键快速选择
        if event.key() >= Qt.Key.Key_1 and event.key() <= Qt.Key.Key_9:
            index = event.key() - Qt.Key.Key_1
            if index < len(self.current_items):
                self._on_paste_item(self.current_items[index].id)
                return
        
        super().keyPressEvent(event)
    
    def resizeEvent(self, event):
        """窗口大小改变时更新列表项宽度"""
        super().resizeEvent(event)
        # 更新所有列表项的宽度
        viewport_width = self.list_widget.viewport().width()
        if viewport_width < 50:
            return
            
        for i in range(self.list_widget.count()):
            item = self.list_widget.item(i)
            widget = self.list_widget.itemWidget(item)
            if item and widget:
                widget.setFixedWidth(viewport_width)
                size = item.sizeHint()
                item.setSizeHint(QSize(viewport_width, size.height()))


# 测试代码
if __name__ == "__main__":
    import sys
    app = QApplication(sys.argv)
    
    # 设置深色主题
    app.setStyle("Fusion")
    
    window = ClipboardWindow()
    window.show()
    
    sys.exit(app.exec())
