# -*- coding: utf-8 -*-
"""
剪贴板管理窗口 - 三列布局（独立窗口）

第1列：导航（分组管理、内容管理）
第2列：列表（分组列表/内容分组选择）
第3列：详细编辑区

这是一个独立的设置窗口，不是剪贴板窗口的子窗口。
使用单例模式确保只有一个实例存在。
"""

from ui.dialogs import show_warning_dialog, show_info_dialog, show_confirm_dialog
from PySide6.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout, QLabel, 
    QPushButton, QFrame, QListWidget, QListWidgetItem,
    QTextEdit, QApplication, QScrollArea,
    QFileDialog, QGridLayout
)
from PySide6.QtCore import Qt, Signal, QSize, QMimeData, QPoint, QTimer
from PySide6.QtGui import QFont, QDrag, QPixmap, QPainter, QColor, QCursor, QPen

from qfluentwidgets import (
    PushButton as FluentPushButton,
    PrimaryPushButton,
    TransparentPushButton,
    NavigationInterface,
    NavigationItemPosition,
    FluentIcon,
    LineEdit,
    ComboBox,
    BodyLabel,
    CaptionLabel,
)

from typing import Optional, List
from core import safe_event

# 支持直接运行和作为模块导入
try:
    from .data_manager import ClipboardManager, Group
    from .emoji_data import get_emoji_groups, get_group_icon
except ImportError:
    from data_manager import ClipboardManager, Group
    from emoji_data import get_emoji_groups, get_group_icon


# 单例实例
_manage_window_instance: Optional['ManageDialog'] = None


def get_manage_dialog(manager: ClipboardManager = None) -> 'ManageDialog':
    """获取管理窗口的单例实例"""
    global _manage_window_instance
    if _manage_window_instance is None:
        if manager is None:
            manager = ClipboardManager()
        _manage_window_instance = ManageDialog(manager)
    return _manage_window_instance


def get_existing_manage_dialog() -> Optional['ManageDialog']:
    """获取已存在的管理窗口实例，不主动创建。"""
    return _manage_window_instance


class DraggableListWidget(QListWidget):
    """自定义列表控件，实现更好的拖拽视觉效果"""
    
    def __init__(self, parent=None):
        super().__init__(parent)
        self._drag_start_row = -1  # 记录拖拽开始的行号
    
    def startDrag(self, supportedActions):
        """重写拖拽开始事件，自定义拖拽样式"""
        item = self.currentItem()
        if not item:
            return
        
        # 记录拖拽开始的行号
        self._drag_start_row = self.row(item)
        
        indexes = self.selectedIndexes()
        if not indexes:
            return
            
        # 1. 准备拖拽数据
        mime_data = self.model().mimeData(indexes)
        drag = QDrag(self)
        drag.setMimeData(mime_data)
        
        # 2. 生成自定义拖拽图片
        # 获取 Item 在视口中的几何位置
        rect = self.visualItemRect(item)
        # 从视口抓取该区域的图像
        pixmap = self.viewport().grab(rect)
        
        # 3. 对图片进行美化处理
        drag_pixmap = QPixmap(pixmap.size())
        drag_pixmap.fill(Qt.GlobalColor.transparent)
        
        painter = QPainter(drag_pixmap)
        painter.setOpacity(0.8)  # 稍微透明一点
        painter.drawPixmap(0, 0, pixmap)
        
        # 绘制选中样式边框，增强视觉反馈
        painter.setPen(QColor("#1976D2"))
        painter.setBrush(Qt.BrushStyle.NoBrush)
        painter.drawRect(0, 0, drag_pixmap.width()-1, drag_pixmap.height()-1)
        
        painter.end()
        
        drag.setPixmap(drag_pixmap)
        
        # 4. 设置热点
        # 计算鼠标相对于 item 左上角的位置
        viewport_pos = self.viewport().mapFromGlobal(QCursor.pos())
        hot_spot = viewport_pos - rect.topLeft()
        drag.setHotSpot(hot_spot)

        # 5. 执行拖拽
        drag.exec(supportedActions)
        self._drag_start_row = -1  # 重置
    
    @safe_event
    def dropEvent(self, event):
        """重写放置事件，禁止拖到第一个位置"""
        # 获取放置位置
        drop_pos = event.position().toPoint() if hasattr(event, 'position') else event.pos()
        drop_item = self.itemAt(drop_pos)
        
        if drop_item:
            drop_row = self.row(drop_item)
            # 计算是放在上方还是下方
            item_rect = self.visualItemRect(drop_item)
            if drop_pos.y() < item_rect.center().y():
                target_row = drop_row
            else:
                target_row = drop_row + 1
        else:
            # 放在空白区域，放到最后
            target_row = self.count()
        
        # 如果目标位置是第一行(索引0)，并且不是从第一行拖过来的，则拒绝
        if target_row == 0 and self._drag_start_row > 0:
            event.ignore()
            return
        
        # 允许放置
        super().dropEvent(event)
    
    @safe_event
    def dragMoveEvent(self, event):
        """重写拖拽移动事件，控制视觉反馈"""
        super().dragMoveEvent(event)
        self.viewport().update()
    
    @safe_event
    def paintEvent(self, event):
        """重写绘制事件，绘制更明显的插入指示线"""
        super().paintEvent(event)
        
        # 检查是否处于拖拽状态
        if self.state() == QListWidget.State.DraggingState:
            painter = QPainter(self.viewport())
            
            # 设置画笔：加粗的蓝色线条
            pen = QPen(QColor("#1976D2"))
            pen.setWidth(3)
            painter.setPen(pen)
            
            # 获取鼠标位置
            pos = self.viewport().mapFromGlobal(QCursor.pos())
            item = self.itemAt(pos)
            
            y = -1
            line_rect = None
            
            if item:
                item_index = self.row(item)
                rect = self.visualItemRect(item)
                
                # 判断是在上半部分还是下半部分
                if pos.y() < rect.center().y():
                    # 在该项上方插入
                    # 禁止插入到第一个item（新建按钮）的上方
                    if item_index > 0:
                        y = rect.top()
                        line_rect = rect
                else:
                    # 在该项下方插入
                    y = rect.bottom()
                    line_rect = rect
            else:
                # 如果没有指向任何item，检查是否在最下面
                if self.count() > 0:
                    last_item = self.item(self.count()-1)
                    rect = self.visualItemRect(last_item)
                    if pos.y() > rect.bottom():
                        y = rect.bottom()
                        line_rect = rect

            if y != -1 and line_rect:
                # 绘制横线
                painter.drawLine(line_rect.left(), y, line_rect.right(), y)
                
                # 绘制两端的小圆点，使其更醒目
                painter.setBrush(QColor("#1976D2"))
                painter.setPen(Qt.PenStyle.NoPen)
                radius = 4
                painter.drawEllipse(QPoint(line_rect.left(), y), radius, radius)
                painter.drawEllipse(QPoint(line_rect.right(), y), radius, radius)
            
            painter.end()


class ManageDialog(QWidget):
    """剪贴板管理窗口 - 三列布局（独立窗口）"""
    
    # 信号 - 用于通知剪贴板窗口更新
    group_added = Signal()
    content_added = Signal(int)
    data_changed = Signal()  # 新增：数据变化信号，统一通知更新
    
    def __init__(self, manager: ClipboardManager, parent=None):
        super().__init__(parent)
        self.manager = manager
        self.current_mode = "group"  # "group" 或 "content"
        self.selected_group_id = None  # 当前选中的分组ID（内容模式用）
        self.editing_group_id = None  # 正在编辑的分组ID
        self.editing_item_id = None  # 正在编辑的内容ID
        
        self.setWindowTitle(self.tr("Clipboard Management"))
        self.setMinimumSize(800, 540)
        self.resize(900, 600)
        # 设置为独立窗口，带有最小化、最大化、关闭按钮
        self.setWindowFlags(
            Qt.WindowType.Window | 
            Qt.WindowType.WindowCloseButtonHint |
            Qt.WindowType.WindowMinimizeButtonHint |
            Qt.WindowType.WindowMaximizeButtonHint
        )
        # 设置窗口图标
        try:
            from core.resource_manager import ResourceManager
            import os
            icon_path = ResourceManager.get_resource_path("svg/托盘.svg")
            if os.path.exists(icon_path):
                from PySide6.QtGui import QIcon
                self.setWindowIcon(QIcon(icon_path))
        except Exception:
            pass
        # 设置窗口属性，关闭时不自动销毁（保持单例）
        self.setAttribute(Qt.WidgetAttribute.WA_DeleteOnClose, False)
        
        self._setup_ui()
        self._switch_mode("group")
        self._center_on_screen()
        
        # 应用 Fluent 风格全局样式（仅作用于非 Fluent 控件）
        self.setStyleSheet("""
            QTextEdit {
                border: 1px solid #E0E0E0;
                border-radius: 6px;
                padding: 8px;
                font-size: 13px;
                background: #FAFAFA;
                color: #333333;
            }
            QTextEdit:focus {
                border-color: #1976D2;
                background: #FFFFFF;
            }
        """)
    
    def _center_on_screen(self):
        """在鼠标所在屏幕居中显示"""
        cursor_pos = QCursor.pos()
        screen = QApplication.screenAt(cursor_pos)
        if not screen:
            screen = QApplication.primaryScreen()
        if screen:
            geo = screen.availableGeometry()
            x = geo.x() + (geo.width() - self.width()) // 2
            y = geo.y() + (geo.height() - self.height()) // 2
            self.move(x, y)
    
    def show_and_activate(self):
        """显示并激活窗口（独立窗口专用方法）"""
        if self.isMinimized():
            self.showNormal()
        else:
            self.show()
        self.raise_()
        self.activateWindow()

    def refresh_after_external_change(self, deleted_group_id: Optional[int] = None):
        """外部数据变化后同步当前管理窗口，避免显示已删除的旧数据。"""
        self.setUpdatesEnabled(False)
        try:
            if deleted_group_id is not None:
                if self.editing_group_id == deleted_group_id:
                    self.editing_group_id = None
                if self.selected_group_id == deleted_group_id:
                    self.selected_group_id = None

            if self.current_mode == "group":
                self._refresh_group_list()
                if self.editing_group_id is not None:
                    self._show_edit_group_form(self.editing_group_id)
                else:
                    self._show_new_group_form()
            elif self.current_mode == "content":
                self._refresh_group_combo()
                self._refresh_content_list()
                if self.editing_item_id is not None and self.manager.get_item(self.editing_item_id) is not None:
                    self._show_edit_content_form(self.editing_item_id)
                else:
                    self.editing_item_id = None
                    self._show_new_content_form()
        finally:
            self.setUpdatesEnabled(True)

    def open_group_editor(self, group_id: int):
        """打开并定位到指定分组的编辑界面"""
        self.setUpdatesEnabled(False)
        if self.current_mode != "group":
            self._switch_mode("group")
        else:
            self._refresh_group_list()
        self.editing_group_id = group_id
        self._select_list_item("group", group_id)
        self._show_edit_group_form(group_id)
        self.setUpdatesEnabled(True)
        self.show_and_activate()

    def open_item_editor(self, item_id: int, group_id: Optional[int]):
        """打开并定位到指定内容的编辑界面"""
        self.setUpdatesEnabled(False)
        if self.current_mode != "content":
            self._switch_mode("content")
        if group_id is not None:
            self._select_group_in_combo(group_id)
        self._refresh_content_list()
        self.editing_item_id = item_id
        self._select_list_item("item", item_id)
        self._show_edit_content_form(item_id)
        self.setUpdatesEnabled(True)
        self.show_and_activate()
    
    @safe_event
    def closeEvent(self, event):
        """关闭事件 - 只隐藏窗口，不销毁"""
        self.hide()
        event.ignore()  # 阻止默认关闭行为
    
    def _setup_ui(self):
        """设置三列布局"""
        
        main_layout = QHBoxLayout(self)
        main_layout.setContentsMargins(0, 0, 0, 0)
        main_layout.setSpacing(0)
        
        # ========== 第1列：导航 ==========
        self.nav_column = self._create_nav_column()
        main_layout.addWidget(self.nav_column)
        
        # ========== 第2列：列表 ==========
        self.list_column = self._create_list_column()
        main_layout.addWidget(self.list_column)
        
        # ========== 第3列：详情 ==========
        self.detail_column = self._create_detail_column()
        main_layout.addWidget(self.detail_column, 1)
    
    def _create_nav_column(self) -> QWidget:
        """创建导航列（使用 Fluent NavigationInterface）"""
        widget = QWidget()
        widget.setFixedWidth(180)
        widget.setStyleSheet("""
            QWidget { background: #F5F6F8; border-right: 1px solid #E8E8E8; }
        """)
        
        layout = QVBoxLayout(widget)
        layout.setContentsMargins(0, 8, 0, 8)
        layout.setSpacing(4)
        
        # 标题
        title = BodyLabel(self.tr("Management"))
        title.setStyleSheet("font-size: 14px; font-weight: 600; padding: 8px 12px 8px 12px; background: transparent;")
        layout.addWidget(title)
        
        # 使用 NavigationInterface
        self.nav_interface = NavigationInterface(
            parent=widget, showMenuButton=False, 
            showReturnButton=False, collapsible=False
        )
        self.nav_interface.setExpandWidth(168)
        self.nav_interface.setMinimumExpandWidth(0)
        self.nav_interface.expand(useAni=False)
        self.nav_interface.setMinimumWidth(168)
        self.nav_interface.setMaximumWidth(176)
        
        self.nav_interface.addItem(
            routeKey="group",
            icon=FluentIcon.FOLDER,
            text=self.tr("Group Management"),
            onClick=lambda: self._switch_mode("group"),
            position=NavigationItemPosition.TOP,
        )
        self.nav_interface.addItem(
            routeKey="content",
            icon=FluentIcon.EDIT,
            text=self.tr("Content Manager"),
            onClick=lambda: self._switch_mode("content"),
            position=NavigationItemPosition.TOP,
        )
        self.nav_interface.addItem(
            routeKey="import_export",
            icon=FluentIcon.DOWNLOAD,
            text=self.tr("Import/Export"),
            onClick=lambda: self._switch_mode("import_export"),
            position=NavigationItemPosition.TOP,
        )
        
        layout.addWidget(self.nav_interface, 1)
        
        # 关闭按钮
        close_btn = FluentPushButton(self.tr("Close"))
        close_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        close_btn.clicked.connect(self.close)
        close_btn_layout = QHBoxLayout()
        close_btn_layout.setContentsMargins(8, 0, 8, 0)
        close_btn_layout.addWidget(close_btn)
        layout.addLayout(close_btn_layout)
        
        return widget
    
    def _create_list_column(self) -> QWidget:
        """创建列表列"""
        widget = QWidget()
        widget.setFixedWidth(220)
        widget.setStyleSheet("""
            QWidget { background: #FAFBFC; border-right: 1px solid #E8E8E8; }
        """)
        
        layout = QVBoxLayout(widget)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)
        
        # 顶部区域
        header = QWidget()
        header.setStyleSheet("background: transparent;")
        header_layout = QVBoxLayout(header)
        header_layout.setContentsMargins(12, 12, 12, 8)
        header_layout.setSpacing(8)
        
        # 列表标题
        self.list_title = BodyLabel(self.tr("Group List"))
        self.list_title.setStyleSheet("font-size: 13px; font-weight: 500; background: transparent;")
        header_layout.addWidget(self.list_title)
        
        # 分组下拉框（内容模式用）
        self.group_combo = ComboBox()
        self.group_combo.currentIndexChanged.connect(self._on_group_combo_changed)
        self.group_combo.hide()
        header_layout.addWidget(self.group_combo)
        
        layout.addWidget(header)
        
        # 列表（启用拖拽排序）
        self.list_widget = DraggableListWidget()
        self.list_widget.setDragDropMode(QListWidget.DragDropMode.InternalMove)
        self.list_widget.setDefaultDropAction(Qt.DropAction.MoveAction)
        self.list_widget.setSelectionMode(QListWidget.SelectionMode.SingleSelection)
        self.list_widget.setStyleSheet("""
            QListWidget {
                background: transparent;
                border: none;
                outline: none;
                color: #333333;
            }
            QListWidget::item {
                padding: 0px 12px;
                border-bottom: 1px solid #F0F0F0;
                color: #333333;
            }
            QListWidget::item:selected {
                background: #E3F2FD;
                color: #1976D2;
            }
            QListWidget::item:hover {
                background: #F0F0F0;
            }
        """)
        self.list_widget.itemClicked.connect(self._on_list_item_clicked)
        # 监听拖拽完成事件
        self.list_widget.model().rowsMoved.connect(self._on_list_reordered)
        layout.addWidget(self.list_widget, 1)
        
        return widget
    
    def _create_detail_column(self) -> QWidget:
        """创建详情列"""
        widget = QWidget()
        widget.setStyleSheet("QWidget { background: #FFFFFF; }")
        
        layout = QVBoxLayout(widget)
        layout.setContentsMargins(24, 20, 24, 20)
        layout.setSpacing(16)
        
        # 详情标题
        self.detail_title = BodyLabel(self.tr("New Group"))
        self.detail_title.setStyleSheet("font-size: 16px; font-weight: 600;")
        layout.addWidget(self.detail_title)
        
        # 分隔线
        line = QFrame()
        line.setFrameShape(QFrame.Shape.HLine)
        line.setStyleSheet("background: #E8E8E8;")
        line.setFixedHeight(1)
        layout.addWidget(line)
        
        # 内容区域（滚动）
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QFrame.Shape.NoFrame)
        scroll.setStyleSheet("QScrollArea { background: transparent; border: none; }")
        
        self.detail_content = QWidget()
        self.detail_content.setStyleSheet("background: transparent;")
        self.detail_layout = QVBoxLayout(self.detail_content)
        self.detail_layout.setContentsMargins(0, 0, 0, 0)
        self.detail_layout.setSpacing(16)
        
        scroll.setWidget(self.detail_content)
        layout.addWidget(scroll, 1)
        
        # 底部按钮区
        self.btn_layout = QHBoxLayout()
        self.btn_layout.setSpacing(12)
        self.btn_layout.addStretch()
        
        self.delete_btn = FluentPushButton(self.tr("Delete"))
        self.delete_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        self.delete_btn.setStyleSheet("""
            PushButton {
                background: #FFEBEE;
                color: #D32F2F;
                border: none;
                border-radius: 6px;
                padding: 10px 24px;
                font-size: 13px;
            }
            PushButton:hover { background: #FFCDD2; }
            PushButton:pressed { background: #EF9A9A; }
        """)
        self.delete_btn.clicked.connect(self._on_delete_clicked)
        self.delete_btn.hide()
        self.btn_layout.addWidget(self.delete_btn)
        
        self.save_btn = PrimaryPushButton(self.tr("Save"))
        self.save_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        self.save_btn.clicked.connect(self._on_save_clicked)
        self.btn_layout.addWidget(self.save_btn)
        
        layout.addLayout(self.btn_layout)
        
        return widget
    
    def _switch_mode(self, mode: str):
        """切换模式"""
        self.current_mode = mode
        self.nav_interface.setCurrentItem(mode)
        
        if mode == "group":
            self.list_column.show()  # 显示中间列
            self.list_title.setText(self.tr("Group List"))
            self.group_combo.hide()
            self.list_widget.show()
            self._refresh_group_list()
            self._show_new_group_form()
        elif mode == "content":
            self.list_column.show()  # 显示中间列
            self.list_title.setText(self.tr("Content List"))
            self.group_combo.show()
            self.list_widget.show()
            self._refresh_group_combo()
            self._refresh_content_list()
            self._show_new_content_form()
        elif mode == "import_export":
            self.list_column.hide()  # 隐藏整个中间列
            self._show_import_export_form()

    def _select_group_in_combo(self, group_id: int):
        """在分组下拉框中选中指定分组"""
        self.group_combo.blockSignals(True)
        try:
            for i in range(self.group_combo.count()):
                if self.group_combo.itemData(i) == group_id:
                    self.group_combo.setCurrentIndex(i)
                    self.selected_group_id = group_id
                    return
        finally:
            self.group_combo.blockSignals(False)

    def _select_list_item(self, item_type: str, item_id: int):
        """在列表中选中指定类型的项"""
        for i in range(self.list_widget.count()):
            item = self.list_widget.item(i)
            data = item.data(Qt.ItemDataRole.UserRole) if item else None
            if data and data[0] == item_type and data[1] == item_id:
                self.list_widget.setCurrentRow(i)
                self.list_widget.scrollToItem(item)
                return
    
    def _refresh_group_combo(self):
        """刷新分组下拉框"""
        self.group_combo.blockSignals(True)
        self.group_combo.clear()
        
        groups = self.manager.get_groups()
        if not groups:
            self.group_combo.addItem(self.tr("(Please create a group first)"), userData=None)
            self.selected_group_id = None
        else:
            for group in groups:
                icon = group.icon or "📁"
                self.group_combo.addItem(f"{icon} {group.name}", userData=group.id)
            
            # 保持之前选中或默认第一个
            idx = 0
            if self.selected_group_id:
                for i in range(self.group_combo.count()):
                    if self.group_combo.itemData(i) == self.selected_group_id:
                        idx = i
                        break
            self.group_combo.setCurrentIndex(idx)
            self.selected_group_id = self.group_combo.currentData()
        
        self.group_combo.blockSignals(False)
    
    def _on_group_combo_changed(self, index: int):
        """分组下拉框改变"""
        self.selected_group_id = self.group_combo.currentData()
        self._refresh_content_list()
        self._show_new_content_form()
    
    def _refresh_group_list(self):
        """刷新分组列表（分组管理模式）"""
        # 保存当前选中的group_id（用于刷新后恢复选择）
        current_item = self.list_widget.currentItem()
        saved_group_id = None
        if current_item:
            data = current_item.data(Qt.ItemDataRole.UserRole)
            if data and data[0] == "group":
                saved_group_id = data[1]
        
        self.list_widget.clear()
        
        # 新建分组项
        new_item = QListWidgetItem(self.tr("New Group"))
        new_item.setData(Qt.ItemDataRole.UserRole, ("new", None))
        self.list_widget.addItem(new_item)
        
        # 已有分组
        groups = self.manager.get_groups()
        restored_selection = False
        for i, group in enumerate(groups):
            icon = group.icon or "📁"
            item = QListWidgetItem(f"{icon} {group.name}")
            item.setData(Qt.ItemDataRole.UserRole, ("group", group.id))
            self.list_widget.addItem(item)
            
            # 恢复之前选中的项
            if saved_group_id is not None and group.id == saved_group_id:
                self.list_widget.setCurrentRow(i + 1)  # +1 因为第0项是"新建分组"
                restored_selection = True
        
        # 如果没有恢复选择（比如是新建或者原来的项被删除了），则选中第一项
        if not restored_selection:
            self.list_widget.setCurrentRow(0)
    
    def _refresh_content_list(self):
        """刷新内容列表（内容管理模式）"""
        # 保存当前选中的item_id（用于刷新后恢复选择）
        current_item = self.list_widget.currentItem()
        saved_item_id = None
        if current_item:
            data = current_item.data(Qt.ItemDataRole.UserRole)
            if data and data[0] == "item":
                saved_item_id = data[1]
        
        self.list_widget.clear()
        
        if self.selected_group_id is None:
            item = QListWidgetItem(self.tr("(Please select a group first)"))
            item.setData(Qt.ItemDataRole.UserRole, (None, None))
            self.list_widget.addItem(item)
            return
        
        # 新建内容项
        new_item = QListWidgetItem(self.tr("Add Content"))
        new_item.setData(Qt.ItemDataRole.UserRole, ("new", None))
        self.list_widget.addItem(new_item)
        
        # 分组内的内容
        items = self.manager.get_by_group(self.selected_group_id, limit=50)
        restored_selection = False
        for i, item in enumerate(items):
            # 优先显示标题，否则显示内容预览
            if item.title:
                display = item.title
            else:
                preview = item.content[:30] + "..." if len(item.content) > 30 else item.content
                display = preview.replace('\n', ' ')
            list_item = QListWidgetItem(f"{display}")
            list_item.setData(Qt.ItemDataRole.UserRole, ("item", item.id))
            self.list_widget.addItem(list_item)
            
            # 恢复之前选中的项
            if saved_item_id is not None and item.id == saved_item_id:
                self.list_widget.setCurrentRow(i + 1)  # +1 因为第0项是"新建内容"
                restored_selection = True
        
        # 如果没有恢复选择（比如是新建或者原来的项被删除了），则选中第一项
        if not restored_selection:
            self.list_widget.setCurrentRow(0)
    
    def _on_list_item_clicked(self, item: QListWidgetItem):
        """列表项点击"""
        data = item.data(Qt.ItemDataRole.UserRole)
        if not data:
            return
        
        item_type, item_id = data
        
        if self.current_mode == "group":
            if item_type == "new":
                self.editing_group_id = None
                self._show_new_group_form()
            elif item_type == "group":
                self.editing_group_id = item_id
                self._show_edit_group_form(item_id)
        else:
            if item_type == "new":
                self.editing_item_id = None
                self._show_new_content_form()
            elif item_type == "item":
                self.editing_item_id = item_id
                self._show_edit_content_form(item_id)
    
    def _on_list_reordered(self, parent, start, end, destination, row):
        """列表拖拽重排序后的处理"""
        # parent: QModelIndex, start: int, end: int, destination: QModelIndex, row: int
        # 注意：row 是新位置的索引
        
        if self.current_mode == "group":
            self._handle_group_reorder(start, row)
        else:
            self._handle_item_reorder(start, row)
    
    def _handle_group_reorder(self, old_index: int, new_index: int):
        """处理分组拖拽排序"""
        from core.logger import log_debug, log_info, log_error
        
        log_debug("拖拽分组", f"old_index={old_index}, new_index={new_index}")
        
        # 获取所有分组（排除第一个"新建"项）
        # 列表顺序是 item_order DESC（大的在前/上面）
        groups = self.manager.get_groups()
        
        log_debug("拖拽分组", f"当前有 {len(groups)} 个分组")
        
        if old_index <= 0 or old_index > len(groups):
            log_error("拖拽分组", f"索引越界: old_index={old_index}")
            return
        
        # 调整索引（减去"新建"项）
        old_pos = old_index - 1
        new_pos = new_index - 1 if new_index > 0 else 0
        
        if old_pos < 0 or old_pos >= len(groups):
            log_error("拖拽分组", f"调整后索引越界: old_pos={old_pos}")
            return
        
        moved_group = groups[old_pos]
        log_debug("拖拽分组", f"移动分组: ID={moved_group.id}, name={moved_group.name}")
        
        # 先把移动项从列表中"移除"，计算它应该插入的位置
        temp_groups = [g for i, g in enumerate(groups) if i != old_pos]
        
        # 调整 new_pos（因为移除了 old_pos）
        adjusted_new_pos = new_pos if new_pos < old_pos else new_pos - 1
        
        # before_id: 界面上在它上面的项（index 更小，order 更大）
        # after_id: 界面上在它下面的项（index 更大，order 更小）
        before_id = None
        after_id = None
        
        if adjusted_new_pos > 0:
            before_id = temp_groups[adjusted_new_pos - 1].id
        
        if adjusted_new_pos < len(temp_groups):
            after_id = temp_groups[adjusted_new_pos].id
        
        log_debug("拖拽分组", f"计算: before_id={before_id}, after_id={after_id}")
        
        try:
            # 调用 Rust 接口
            self.manager._manager.move_group_between(
                moved_group.id,
                before_id=before_id,
                after_id=after_id
            )
            log_info("拖拽分组", f"移动成功: ID={moved_group.id}, before={before_id}, after={after_id}")
            
            # 刷新列表显示新顺序
            self._refresh_group_list()
            
            # 发送分组变化信号（触发面板按钮刷新）
            self.group_added.emit()
            
            # 注意：分组排序变化不发送 data_changed 信号
            # 因为这只是分组按钮顺序调整，不影响历史记录内容
            
        except Exception as e:
            log_error("拖拽分组", f"移动失败: {e}")
            import traceback
            traceback.print_exc()
            # 失败时刷新列表恢复原顺序
            self._refresh_group_list()
    
    def _handle_item_reorder(self, old_index: int, new_index: int):
        """处理内容拖拽排序"""
        from core.logger import log_debug, log_info, log_error
        
        log_debug("拖拽内容", f"old_index={old_index}, new_index={new_index}")
        
        if self.selected_group_id is None:
            log_error("拖拽内容", "未选择分组")
            return
        
        # 获取当前分组的内容（返回的直接是列表）
        # 列表顺序是 item_order DESC（大的在前/上面）
        items = self.manager.get_by_group(self.selected_group_id, offset=0, limit=1000)
        
        log_debug("拖拽内容", f"当前分组有 {len(items)} 个内容")
        
        if old_index <= 0 or old_index > len(items):
            log_error("拖拽内容", f"索引越界: old_index={old_index}, items count={len(items)}")
            return
        
        # 调整索引（减去"新建"项）
        old_pos = old_index - 1
        new_pos = new_index - 1 if new_index > 0 else 0
        
        if old_pos < 0 or old_pos >= len(items):
            log_error("拖拽内容", f"调整后索引越界: old_pos={old_pos}")
            return
        
        moved_item = items[old_pos]
        log_debug("拖拽内容", f"移动项: ID={moved_item.id}, title={moved_item.title or moved_item.content[:20]}")
        
        # 先把移动项从列表中"移除"，计算它应该插入的位置
        # 创建一个不包含移动项的列表来计算邻居
        temp_items = [item for i, item in enumerate(items) if i != old_pos]
        
        # 调整 new_pos（因为移除了 old_pos）
        adjusted_new_pos = new_pos if new_pos < old_pos else new_pos - 1
        
        # before_id: 界面上在它上面的项（index 更小，order 更大）
        # after_id: 界面上在它下面的项（index 更大，order 更小）
        before_id = None
        after_id = None
        
        if adjusted_new_pos > 0:
            # 上面有项
            before_id = temp_items[adjusted_new_pos - 1].id
        
        if adjusted_new_pos < len(temp_items):
            # 下面有项
            after_id = temp_items[adjusted_new_pos].id
        
        log_debug("拖拽内容", f"计算: before_id={before_id}, after_id={after_id}")
        
        try:
            # 调用 Rust 接口
            self.manager._manager.move_item_between(
                moved_item.id,
                before_id=before_id,
                after_id=after_id
            )
            log_info("拖拽内容", f"移动成功: ID={moved_item.id}, before={before_id}, after={after_id}")
            
            # 刷新列表显示新顺序
            self._refresh_content_list()
            
            # 注意：分组内容排序变化不发送 data_changed 信号
            # 因为这只是内部顺序调整，不影响主窗口（历史记录）
            
        except Exception as e:
            log_error("拖拽内容", f"移动失败: {e}")
            import traceback
            traceback.print_exc()
            # 失败时刷新列表恢复原顺序
            self._refresh_content_list()
            
        except Exception as e:
            print(f"❌ 移动内容失败: {e}")
            # 失败时刷新列表恢复原顺序
            self._refresh_content_list()
    
    def _clear_detail_layout(self):
        """清空详情区域"""
        # 重置图标按钮列表
        self.icon_buttons = []
        # 递归删除所有子控件和子布局
        def clear_layout(layout):
            while layout.count():
                child = layout.takeAt(0)
                widget = child.widget()
                if widget:
                    widget.setParent(None)
                    widget.deleteLater()
                elif child.layout():
                    clear_layout(child.layout())
        clear_layout(self.detail_layout)
    
    def _show_new_group_form(self):
        """显示新建分组表单"""
        self._clear_detail_layout()
        self.detail_title.setText(self.tr("New Group"))
        self.delete_btn.hide()
        self.save_btn.show()
        self.save_btn.setText(self.tr("Create"))
        self.editing_group_id = None
        
        # 分组名称
        name_label = BodyLabel(self.tr("Group Name"))
        self.detail_layout.addWidget(name_label)
        
        self.group_name_input = LineEdit()
        self.group_name_input.setPlaceholderText(self.tr("Enter group name..."))
        self.detail_layout.addWidget(self.group_name_input)
        
        # 选择图标
        icon_label = BodyLabel(self.tr("Select Icon"))
        self.detail_layout.addWidget(icon_label)
        
        # 创建 emoji 选择器
        self._create_emoji_picker()
    
    def _create_emoji_picker(self, current_icon: str = "📁"):
        """创建 emoji 选择器（输入框 + 预览 + 分组标签页 + 可滚动网格）"""
        
        # ── 第一行：输入框 + 预览 ──
        input_row = QHBoxLayout()
        input_row.setSpacing(12)
        
        self.icon_input = LineEdit()
        self.icon_input.setPlaceholderText(self.tr("Enter or paste emoji..."))
        self.icon_input.setText(current_icon)
        self.icon_input.setMaxLength(4)
        self.icon_input.setStyleSheet("font-size: 18px; min-width: 120px; max-width: 150px;")
        self.icon_input.textChanged.connect(self._on_icon_input_changed)
        input_row.addWidget(self.icon_input)
        
        preview_label = CaptionLabel(self.tr("Preview:"))
        input_row.addWidget(preview_label)
        
        self.icon_preview = QLabel(current_icon)
        self.icon_preview.setFixedSize(48, 48)
        self.icon_preview.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.icon_preview.setStyleSheet("""
            QLabel {
                font-size: 28px;
                background: #F5F5F5;
                border: 2px solid #E0E0E0;
                border-radius: 8px;
            }
        """)
        input_row.addWidget(self.icon_preview)
        input_row.addStretch()
        self.detail_layout.addLayout(input_row)
        
        # ── 分组标签栏 ──
        group_order, emoji_groups = get_emoji_groups()
        
        tab_bar = QHBoxLayout()
        tab_bar.setSpacing(0)
        tab_bar.setContentsMargins(0, 4, 0, 0)
        self._emoji_tab_buttons: list[QPushButton] = []
        self._emoji_group_order = group_order
        self._emoji_groups = emoji_groups
        
        for idx, group_name in enumerate(group_order):
            icon_char = get_group_icon(group_name)
            btn = QPushButton(icon_char)
            btn.setFixedSize(36, 30)
            btn.setCursor(Qt.CursorShape.PointingHandCursor)
            btn.setToolTip(group_name)
            btn.setCheckable(True)
            btn.setStyleSheet(self._emoji_tab_style(False))
            btn.clicked.connect(lambda checked, i=idx: self._switch_emoji_group(i))
            tab_bar.addWidget(btn)
            self._emoji_tab_buttons.append(btn)
        tab_bar.addStretch()
        self.detail_layout.addLayout(tab_bar)
        
        # ── 可滚动 emoji 网格区域（无外边框，紧贴 tab 栏） ──
        self._emoji_scroll = QScrollArea()
        self._emoji_scroll.setWidgetResizable(True)
        self._emoji_scroll.setMinimumHeight(120)
        self._emoji_scroll.setHorizontalScrollBarPolicy(
            Qt.ScrollBarPolicy.ScrollBarAlwaysOff
        )
        self._emoji_scroll.setStyleSheet("""
            QScrollArea { border: none; background: transparent; }
            QScrollBar:vertical { width: 6px; background: transparent; }
            QScrollBar::handle:vertical { background: #CCC; border-radius: 3px; min-height: 20px; }
            QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; }
        """)
        # stretch=1 让 emoji 区域占满详情列的剩余高度，避免外层出现滚动条
        self.detail_layout.addWidget(self._emoji_scroll, 1)
        
        # 记录当前分组索引，延迟到 show 后再填充网格
        self._emoji_current_idx = 0
        # 用 singleShot 确保布局完成后再填充（此时 viewport 宽度正确）
        QTimer.singleShot(0, lambda: self._switch_emoji_group(0))
    
    @staticmethod
    def _emoji_tab_style(active: bool) -> str:
        """返回 emoji 分组 tab 按钮样式"""
        if active:
            return """
                QPushButton {
                    background: #E3F2FD; border: none;
                    border-bottom: 2px solid #1E88E5;
                    border-radius: 0px; font-size: 18px;
                    padding: 2px 0px;
                }
            """
        return """
            QPushButton {
                background: transparent; border: none;
                border-bottom: 2px solid transparent;
                border-radius: 0px; font-size: 18px;
                padding: 2px 0px;
            }
            QPushButton:hover { background: #F5F5F5; }
        """
    
    @staticmethod
    def _emoji_btn_style() -> str:
        """返回 emoji 网格按钮样式"""
        return """
            QPushButton {
                background: transparent; border: none;
                border-radius: 4px; font-size: 22px; padding: 0px;
            }
            QPushButton:hover {
                background: #E3F2FD;
            }
            QPushButton:pressed {
                background: #BBDEFB;
            }
        """
    
    def _switch_emoji_group(self, group_idx: int):
        """切换 emoji 分组"""
        self._emoji_current_idx = group_idx
        # 更新 tab 高亮
        for i, btn in enumerate(self._emoji_tab_buttons):
            btn.setChecked(i == group_idx)
            btn.setStyleSheet(self._emoji_tab_style(i == group_idx))
        
        group_name = self._emoji_group_order[group_idx]
        emojis = self._emoji_groups[group_name]
        
        # 根据 scroll area 实际可用宽度计算列数
        btn_size = 36
        spacing = 2
        avail_w = self._emoji_scroll.viewport().width() - 4  # 减去边距
        if avail_w < btn_size * 2:
            # viewport 宽度还没就绪（布局未完成），用合理的回退值
            avail_w = 400
        cols = max(1, avail_w // (btn_size + spacing))
        
        # 构建网格容器
        container = QWidget()
        # 限制 container 最大宽度，防止它的 sizeHint 撑宽 scroll area
        container.setMaximumWidth(avail_w + 4)
        grid = QGridLayout(container)
        grid.setSpacing(spacing)
        grid.setContentsMargins(2, 2, 2, 2)
        
        btn_style = self._emoji_btn_style()
        for i, em in enumerate(emojis):
            btn = QPushButton(em)
            btn.setFixedSize(btn_size, btn_size)
            btn.setCursor(Qt.CursorShape.PointingHandCursor)
            btn.setStyleSheet(btn_style)
            btn.clicked.connect(lambda checked, ic=em: self._on_preset_icon_clicked(ic))
            grid.addWidget(btn, i // cols, i % cols)
        
        # 右侧和底部弹性填充，保持靠左对齐
        grid.setColumnStretch(cols, 1)
        grid.setRowStretch(len(emojis) // cols + 1, 1)
        
        self._emoji_scroll.setWidget(container)
    
    def _on_icon_input_changed(self, text: str):
        """输入框内容变化时更新预览（只保留第一个 emoji）"""
        if not text.strip():
            self.icon_preview.setText("📁")
            return
        
        first_char = ""
        for char in text:
            if char.isspace():
                continue
            first_char = char
            break
        
        if first_char:
            if len(text.strip()) > len(first_char):
                self.icon_input.blockSignals(True)
                self.icon_input.setText(first_char)
                self.icon_input.blockSignals(False)
            self.icon_preview.setText(first_char)
        else:
            self.icon_preview.setText("📁")
    
    def _on_preset_icon_clicked(self, icon: str):
        """点击预设图标"""
        self.icon_input.setText(icon)
        self.icon_preview.setText(icon)
    
    def _show_edit_group_form(self, group_id: int):
        """显示编辑分组表单"""
        self._clear_detail_layout()
        self.detail_title.setText(self.tr("Edit Group"))
        self.delete_btn.show()
        self.save_btn.show()
        self.save_btn.setText(self.tr("Save"))
        
        # 获取分组信息
        groups = self.manager.get_groups()
        group = next((g for g in groups if g.id == group_id), None)
        if not group:
            return
        
        # 分组名称
        name_label = BodyLabel(self.tr("Group Name"))
        self.detail_layout.addWidget(name_label)
        
        self.group_name_input = LineEdit()
        self.group_name_input.setText(group.name)
        self.detail_layout.addWidget(self.group_name_input)
        
        # 选择图标
        icon_label = BodyLabel(self.tr("Select Icon"))
        self.detail_layout.addWidget(icon_label)
        
        # 使用 emoji 选择器（传入当前图标）
        current_icon = group.icon or "📁"
        self._create_emoji_picker(current_icon)
    
    def _show_new_content_form(self):
        """显示新建内容表单"""
        self._clear_detail_layout()
        self.detail_title.setText(self.tr("Add Content"))
        self.delete_btn.hide()
        self.save_btn.show()
        self.save_btn.setText(self.tr("Add"))
        self.editing_item_id = None
        
        if self.selected_group_id is None:
            hint = CaptionLabel(self.tr("Please select a group above, or create a group first"))
            self.detail_layout.addWidget(hint)
            self.detail_layout.addStretch()
            return
        
        # 标题输入
        title_label = BodyLabel(self.tr("Title"))
        self.detail_layout.addWidget(title_label)
        
        self.title_input = LineEdit()
        self.title_input.setPlaceholderText(self.tr("Enter title (e.g., Restart Command)..."))
        self.detail_layout.addWidget(self.title_input)
        
        # 内容输入
        content_label = BodyLabel(self.tr("Content"))
        self.detail_layout.addWidget(content_label)
        
        self.content_edit = QTextEdit()
        self.content_edit.setPlaceholderText(self.tr("Enter text content to save..."))
        self.content_edit.setLineWrapMode(QTextEdit.LineWrapMode.NoWrap)
        self.content_edit.setMinimumHeight(180)
        self.detail_layout.addWidget(self.content_edit, 1)
    
    def _show_edit_content_form(self, item_id: int):
        """显示编辑内容表单"""
        self._clear_detail_layout()
        self.detail_title.setText(self.tr("Edit Content"))
        self.delete_btn.show()
        self.save_btn.show()
        self.save_btn.setText(self.tr("Save"))
        self.editing_item_id = item_id
        
        # 获取内容
        item = self.manager.get_item(item_id)
        if not item:
            return
        
        # 标题输入
        title_label = BodyLabel(self.tr("Title"))
        self.detail_layout.addWidget(title_label)
        
        self.title_input = LineEdit()
        self.title_input.setText(item.title or "")
        self.title_input.setPlaceholderText(self.tr("Enter title..."))
        self.detail_layout.addWidget(self.title_input)
        
        # 内容输入
        content_label = BodyLabel(self.tr("Content"))
        self.detail_layout.addWidget(content_label)
        
        self.content_edit = QTextEdit()
        self.content_edit.setText(item.content)
        self.content_edit.setLineWrapMode(QTextEdit.LineWrapMode.NoWrap)
        self.content_edit.setMinimumHeight(180)
        self.detail_layout.addWidget(self.content_edit, 1)
        
        # 创建时间
        if item.created_at:
            time_str = item.created_at.strftime('%Y-%m-%d %H:%M:%S')
            time_label = CaptionLabel(f"{self.tr('Created')}: {time_str}")
            self.detail_layout.addWidget(time_label)
    
    def _show_import_export_form(self):
        """显示导入导出表单"""
        self._clear_detail_layout()
        self.detail_title.setText(self.tr("Import/Export"))
        self.delete_btn.hide()
        self.save_btn.hide()  # 隐藏保存按钮
        
        # 导出区域
        export_section = BodyLabel(self.tr("Export"))
        export_section.setStyleSheet("font-size: 14px; font-weight: 600; margin-top: 8px;")
        self.detail_layout.addWidget(export_section)
        
        export_desc = CaptionLabel(self.tr("Export all saved content to CSV file"))
        self.detail_layout.addWidget(export_desc)
        
        # 编码选择
        encoding_layout = QHBoxLayout()
        encoding_label = CaptionLabel(self.tr("Encoding:"))
        encoding_layout.addWidget(encoding_label)
        
        self.export_encoding_combo = ComboBox()
        self.export_encoding_combo.addItem("UTF-8 (BOM)", userData="utf-8-sig")
        self.export_encoding_combo.addItem("UTF-8", userData="utf-8")
        self.export_encoding_combo.addItem("Shift_JIS", userData="shift_jis")
        self.export_encoding_combo.addItem("GBK", userData="gbk")
        encoding_layout.addWidget(self.export_encoding_combo)
        encoding_layout.addStretch()
        self.detail_layout.addLayout(encoding_layout)
        
        export_btn = PrimaryPushButton(self.tr("📤 Export to CSV"))
        export_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        export_btn.clicked.connect(self._export_to_csv)
        self.detail_layout.addWidget(export_btn)
        
        # 分隔线
        line = QFrame()
        line.setFrameShape(QFrame.Shape.HLine)
        line.setStyleSheet("background: #E8E8E8; margin: 16px 0;")
        line.setFixedHeight(1)
        self.detail_layout.addWidget(line)
        
        # 导入区域
        import_section = BodyLabel(self.tr("Import"))
        import_section.setStyleSheet("font-size: 14px; font-weight: 600; margin-top: 8px;")
        self.detail_layout.addWidget(import_section)
        
        import_desc = CaptionLabel(self.tr("Import content from CSV file (Group, Content, Title)"))
        self.detail_layout.addWidget(import_desc)
        
        import_btn = PrimaryPushButton(self.tr("📥 Import from CSV"))
        import_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        import_btn.clicked.connect(self._import_from_csv)
        self.detail_layout.addWidget(import_btn)
        
        # CSV 格式说明
        format_section = BodyLabel(self.tr("CSV Format"))
        format_section.setStyleSheet("font-size: 14px; font-weight: 600; margin-top: 24px;")
        self.detail_layout.addWidget(format_section)
        
        format_desc = CaptionLabel(
            self.tr("Column 1: Group Name") + "\n" +
            self.tr("Column 2: Content") + "\n" +
            self.tr("Column 3: Title (optional)")
        )
        self.detail_layout.addWidget(format_desc)
        
        self.detail_layout.addStretch()
    
    def _select_icon(self, btn: QPushButton):
        """选择图标（旧方法，保留兼容）"""
        for b in self.icon_buttons:
            b.setChecked(b == btn)
    
    def _get_selected_icon(self) -> str:
        """获取选中的图标（优先从输入框获取，确保只有一个字符）"""
        if hasattr(self, 'icon_input') and self.icon_input:
            text = self.icon_input.text().strip()
            if text:
                # 只取第一个字符
                for char in text:
                    if not char.isspace():
                        return char
        # 回退：从按钮获取
        for btn in self.icon_buttons:
            if btn.isChecked():
                return btn.text()
        return "📁"

    def _get_delete_group_confirm_message(self) -> str:
        return "\n".join([
            self.tr("Are you sure you want to delete this group?"),
            self.tr("All items in the group will also be deleted."),
        ])

    def _group_name_exists(self, name: str, exclude_group_id: Optional[int] = None) -> bool:
        for group in self.manager.get_groups():
            if exclude_group_id is not None and group.id == exclude_group_id:
                continue
            if group.name == name:
                return True
        return False

    @staticmethod
    def _make_unique_group_name(base_name: str, used_names: set[str]) -> str:
        if base_name not in used_names:
            return base_name

        index = 1
        while True:
            candidate = f"{base_name} ({index})"
            if candidate not in used_names:
                return candidate
            index += 1
    
    def _on_save_clicked(self):
        """保存按钮点击"""
        if self.current_mode == "group":
            self._save_group()
        else:
            self._save_content()
    
    def _save_group(self):
        """保存分组"""
        name = self.group_name_input.text().strip()
        if not name:
            show_warning_dialog(self, self.tr("Hint"), self.tr("Please enter group name"))
            return

        if self._group_name_exists(name, exclude_group_id=self.editing_group_id):
            show_warning_dialog(self, self.tr("Hint"), self.tr("A group with this name already exists"))
            return
        
        icon = self._get_selected_icon()
        
        if self.editing_group_id is None:
            # 新建分组
            group_id = self.manager.create_group(name, icon=icon)
            if group_id:
                self.group_added.emit()
                self.data_changed.emit()  # 通知数据变化
                self._refresh_group_list()
                self.group_name_input.clear()
                self.list_widget.setCurrentRow(0)
            else:
                show_warning_dialog(self, self.tr("Failed"), self.tr("Failed to create group"))
        else:
            # 更新分组（名称和图标）
            if self.manager.update_group(self.editing_group_id, name, icon=icon):
                self.group_added.emit()
                self.data_changed.emit()  # 通知数据变化
                self._refresh_group_list()
            else:
                show_warning_dialog(self, self.tr("Failed"), self.tr("Failed to update group"))
    
    def _save_content(self):
        """保存内容"""
        if self.selected_group_id is None:
            show_warning_dialog(self, self.tr("Hint"), self.tr("Please select a group first"))
            return
        
        content = self.content_edit.toPlainText().strip()
        if not content:
            show_warning_dialog(self, self.tr("Hint"), self.tr("Please enter content"))
            return
        
        # 获取标题（可选）
        title = self.title_input.text().strip() if hasattr(self, 'title_input') else None
        title = title if title else None  # 空字符串转为 None
        
        if self.editing_item_id is None:
            # 新建内容
            item_id = self.manager.add_item(content, "text", title=title)
            if item_id:
                if self.manager.move_to_group(item_id, self.selected_group_id):
                    self.content_edit.clear()
                    if hasattr(self, 'title_input'):
                        self.title_input.clear()
                    self.content_added.emit(self.selected_group_id)
                    self.data_changed.emit()  # 通知数据变化
                    self._refresh_content_list()
                    self.list_widget.setCurrentRow(0)
                else:
                    show_warning_dialog(self, self.tr("Failed"), self.tr("Failed to move to group"))
            else:
                show_warning_dialog(self, self.tr("Failed"), self.tr("Failed to add content"))
        else:
            # 编辑内容（使用 update_item）
            if self.manager.update_item(self.editing_item_id, content, title=title):
                self.content_added.emit(self.selected_group_id)
                self.data_changed.emit()  # 通知数据变化
                self._refresh_content_list()
            else:
                show_warning_dialog(self, self.tr("Failed"), self.tr("Failed to update content"))
    
    def _on_delete_clicked(self):
        """删除按钮点击"""
        if self.current_mode == "group" and self.editing_group_id:
            reply = show_confirm_dialog(
                self,
                self.tr("Confirm Delete"),
                self._get_delete_group_confirm_message()
            )
            if reply:
                if self.manager.delete_group(self.editing_group_id):
                    self.editing_group_id = None
                    self.group_added.emit()
                    self.data_changed.emit()  # 通知数据变化
                    self._refresh_group_list()
                    self._show_new_group_form()
                else:
                    show_warning_dialog(self, self.tr("Failed"), self.tr("Failed to delete group"))
        
        elif self.current_mode == "content" and self.editing_item_id:
            reply = show_confirm_dialog(
                self,
                self.tr("Confirm Delete"),
                self.tr("Are you sure you want to delete this item?")
            )
            if reply:
                if self.manager.delete_item(self.editing_item_id):
                    self.editing_item_id = None
                    self.content_added.emit(self.selected_group_id)
                    self.data_changed.emit()  # 通知数据变化
                    self._refresh_content_list()
                    self._show_new_content_form()
                else:
                    show_warning_dialog(self, self.tr("Failed"), self.tr("Failed to delete item"))
    
    def _switch_page(self, index: int):
        """切换页面。"""
        if index == 0:
            self._switch_mode("group")
        else:
            self._switch_mode("content")
    
    def _export_to_csv(self):
        """导出收藏内容到 CSV 文件（仅导出纯文本内容）"""
        import csv
        
        # 选择保存路径
        file_path, _ = QFileDialog.getSaveFileName(
            self,
            self.tr("Export to CSV"),
            "clipboard_export.csv",
            "CSV Files (*.csv);;All Files (*)"
        )
        
        if not file_path:
            return
        
        try:
            rows = []

            # 1. 导出各分组内容（分页避免内存过大）
            groups = self.manager.get_groups()
            for group in groups:
                offset = 0
                page = 500
                while True:
                    items = self.manager.get_by_group(group.id, offset=offset, limit=page)
                    if not items:
                        break
                    for item in items:
                        if item.content_type == "text" and item.content:
                            rows.append([
                                group.name,
                                item.content,
                                item.title or ""
                            ])
                    if len(items) < page:
                        break
                    offset += page
            
            # 获取选择的编码
            encoding = 'utf-8-sig'
            if hasattr(self, 'export_encoding_combo'):
                encoding = self.export_encoding_combo.currentData() or 'utf-8-sig'
            
            with open(file_path, 'w', newline='', encoding=encoding, errors='replace') as f:
                writer = csv.writer(f)
                writer.writerow([self.tr("Group"), self.tr("Content"), self.tr("Title")])
                writer.writerows(rows)
            
            show_info_dialog(
                self,
                self.tr("Export Successful"),
                self.tr("Exported {count} items to CSV file.").format(count=len(rows))
            )
        
        except Exception as e:
            show_warning_dialog(
                self,
                self.tr("Export Failed"),
                self.tr("Failed to export: {error}").format(error=str(e))
            )
    
    def _import_from_csv(self):
        """从 CSV 文件导入内容"""
        import csv
        
        # 选择文件
        file_path, _ = QFileDialog.getOpenFileName(
            self,
            self.tr("Import from CSV"),
            "",
            "CSV Files (*.csv);;All Files (*)"
        )
        
        if not file_path:
            return
        
        try:
            # 尝试多种编码读取 CSV 文件
            rows = []
            content = None
            
            # 按优先级尝试不同编码
            encodings = ['utf-8-sig', 'utf-8', 'shift_jis', 'cp932', 'gbk', 'gb2312', 'gb18030', 'cp1252', 'latin-1']
            for encoding in encodings:
                try:
                    with open(file_path, 'r', newline='', encoding=encoding) as f:
                        content = f.read()
                    break
                except (UnicodeDecodeError, LookupError):
                    continue
            
            if content is None:
                # 最后尝试用 errors='replace' 强制读取
                with open(file_path, 'r', newline='', encoding='utf-8', errors='replace') as f:
                    content = f.read()
            
            # 解析 CSV
            import io
            reader = csv.reader(io.StringIO(content))
            # 跳过标题行
            next(reader, None)
            for row in reader:
                    if len(row) >= 2:
                        group_name = row[0].strip()
                        content = row[1] if len(row) > 1 else ""
                        title = row[2].strip() if len(row) > 2 else ""
                        if group_name and content:
                            rows.append((group_name, content, title))
            
            if not rows:
                show_warning_dialog(
                    self,
                    self.tr("Import Failed"),
                    self.tr("No valid data found in CSV file.")
                )
                return
            
            # 获取现有分组。导入时若同名分组已存在，直接复用该分组。
            existing_groups = self.manager.get_groups()
            existing_group_name_to_id = {g.name: g.id for g in existing_groups}
            import_group_name_to_target: dict[str, int] = {}
            
            # 导入数据
            # 从后往前遍历：Excel 最下面的行最先添加，最上面的行最后添加
            # 这样查询时（ORDER BY item_order DESC），Excel 上面的行会显示在列表上面
            imported_count = 0
            for group_name, content, title in reversed(rows):
                if group_name not in import_group_name_to_target:
                    existing_group_id = existing_group_name_to_id.get(group_name)
                    if existing_group_id is not None:
                        import_group_name_to_target[group_name] = existing_group_id
                    else:
                        new_group_id = self.manager.create_group(group_name)
                        if not new_group_id:
                            continue
                        existing_group_name_to_id[group_name] = new_group_id
                        import_group_name_to_target[group_name] = new_group_id

                group_id = import_group_name_to_target[group_name]
                
                # 添加内容
                title_param = title if title else None
                item_id = self.manager.add_item(content, "text", title=title_param)
                if item_id:
                    self.manager.move_to_group(item_id, group_id)
                    imported_count += 1
            
            # 刷新界面
            self.group_added.emit()
            self.data_changed.emit()  # 通知数据变化
            if self.current_mode == "group":
                self._refresh_group_list()
            else:
                self._refresh_content_list()
            
            show_info_dialog(
                self,
                self.tr("Import Successful"),
                self.tr("Imported {count} items.").format(count=imported_count)
            )
        
        except Exception as e:
            show_warning_dialog(
                self,
                self.tr("Import Failed"),
                self.tr("Failed to import: {error}").format(error=str(e))
            )


if __name__ == "__main__":
    import sys
    app = QApplication(sys.argv)
    manager = ClipboardManager()
    dialog = get_manage_dialog(manager)  # 使用单例函数
    dialog.show()
    sys.exit(app.exec()) 