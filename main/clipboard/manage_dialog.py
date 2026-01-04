# -*- coding: utf-8 -*-
"""
剪贴板管理对话框 - 三列布局

第1列：导航（分组管理、内容管理）
第2列：列表（分组列表/内容分组选择）
第3列：详细编辑区
"""

from PyQt6.QtWidgets import (
    QDialog, QWidget, QVBoxLayout, QHBoxLayout, QLabel, 
    QLineEdit, QPushButton, QFrame, QListWidget, QListWidgetItem,
    QTextEdit, QMessageBox, QApplication, QScrollArea, QComboBox
)
from PyQt6.QtCore import Qt, pyqtSignal, QSize
from PyQt6.QtGui import QFont

from typing import Optional, List

# 支持直接运行和作为模块导入
try:
    from .manager import ClipboardManager, Group
except ImportError:
    from manager import ClipboardManager, Group


class ManageDialog(QDialog):
    """剪贴板管理对话框 - 三列布局"""
    
    # 信号
    group_added = pyqtSignal()
    content_added = pyqtSignal(int)
    
    # 图标选项（6行 x 8列 = 48个）
    ICONS = [
        "\U0001F4C1", "\u2B50", "\u2764", "\U0001F4CC", "\U0001F516", "\U0001F4BC", "\U0001F3AF", "\U0001F4A1",
        "\U0001F525", "\U0001F4C2", "\U0001F4CB", "\U0001F4DD", "\U0001F4CE", "\U0001F4CA", "\U0001F4C8", "\U0001F4BB",
        "\u2705", "\u274C", "\u26A0", "\u2753", "\u2757", "\U0001F534", "\U0001F7E2", "\U0001F535",
        "\U0001F4C5", "\u23F0", "\u231B", "\U0001F4E7", "\U0001F4AC", "\U0001F4DE", "\U0001F514", "\U0001F4E2",
        "\U0001F3AE", "\U0001F3B5", "\U0001F3AC", "\U0001F4F7", "\U0001F3A8", "\U0001F3E0", "\U0001F680", "\U0001F4B0",
        "\U0001F381", "\U0001F527", "\U0001F511", "\U0001F6D2", "\U0001F4E6", "\U0001F31F", "\U0001F48E", "\U0001F340",
    ]
    
    def __init__(self, manager: ClipboardManager, parent=None):
        super().__init__(parent)
        self.manager = manager
        self.current_mode = "group"  # "group" 或 "content"
        self.selected_group_id = None  # 当前选中的分组ID（内容模式用）
        self.editing_group_id = None  # 正在编辑的分组ID
        self.editing_item_id = None  # 正在编辑的内容ID
        
        self.setWindowTitle(self.tr("Clipboard Management"))
        self.setMinimumSize(800, 500)
        self.resize(900, 550)
        self.setWindowFlags(Qt.WindowType.Dialog | Qt.WindowType.WindowCloseButtonHint)
        
        self._setup_ui()
        self._switch_mode("group")
        self._center_on_screen()
    
    def _center_on_screen(self):
        """居中显示"""
        screen = QApplication.primaryScreen()
        if screen:
            geo = screen.availableGeometry()
            x = (geo.width() - self.width()) // 2 + geo.x()
            y = (geo.height() - self.height()) // 2 + geo.y()
            self.move(x, y)
    
    def _setup_ui(self):
        """设置三列布局"""
        self.setStyleSheet("""
            QDialog { background: #FFFFFF; }
            QLineEdit {
                border: 1px solid #E0E0E0;
                border-radius: 6px;
                padding: 8px 12px;
                font-size: 13px;
                background: #FAFAFA;
            }
            QLineEdit:focus {
                border-color: #1976D2;
                background: #FFFFFF;
            }
            QTextEdit {
                border: 1px solid #E0E0E0;
                border-radius: 6px;
                padding: 8px;
                font-size: 13px;
                background: #FAFAFA;
            }
            QTextEdit:focus {
                border-color: #1976D2;
                background: #FFFFFF;
            }
        """)
        
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
        """创建导航列"""
        widget = QWidget()
        widget.setFixedWidth(140)
        widget.setStyleSheet("""
            QWidget { background: #F5F6F8; border-right: 1px solid #E8E8E8; }
        """)
        
        layout = QVBoxLayout(widget)
        layout.setContentsMargins(8, 16, 8, 16)
        layout.setSpacing(4)
        
        # 标题
        title = QLabel(self.tr("Management"))
        title.setStyleSheet("font-size: 14px; font-weight: 600; color: #333; padding: 8px 8px 16px 8px; background: transparent;")
        layout.addWidget(title)
        
        # 分组管理按钮
        self.nav_group_btn = QPushButton(self.tr("📁 Group Management"))
        self.nav_group_btn.setCheckable(True)
        self.nav_group_btn.setChecked(True)
        self.nav_group_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        self.nav_group_btn.setStyleSheet(self._get_nav_btn_style())
        self.nav_group_btn.clicked.connect(lambda: self._switch_mode("group"))
        layout.addWidget(self.nav_group_btn)
        
        # 内容管理按钮
        self.nav_content_btn = QPushButton(self.tr("✏️ Content Manager"))
        self.nav_content_btn.setCheckable(True)
        self.nav_content_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        self.nav_content_btn.setStyleSheet(self._get_nav_btn_style())
        self.nav_content_btn.clicked.connect(lambda: self._switch_mode("content"))
        layout.addWidget(self.nav_content_btn)
        
        layout.addStretch()
        
        # 关闭按钮
        close_btn = QPushButton(self.tr("Close"))
        close_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        close_btn.setStyleSheet("""
            QPushButton {
                background: transparent;
                color: #666;
                border: 1px solid #DDD;
                border-radius: 6px;
                padding: 8px;
                font-size: 12px;
            }
            QPushButton:hover { background: #EEE; }
        """)
        close_btn.clicked.connect(self.close)
        layout.addWidget(close_btn)
        
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
        self.list_title = QLabel(self.tr("Group List"))
        self.list_title.setStyleSheet("""
            font-size: 13px; font-weight: 500; color: #333; background: transparent;
        """)
        header_layout.addWidget(self.list_title)
        
        # 分组下拉框（内容模式用）
        self.group_combo = QComboBox()
        self.group_combo.setStyleSheet("""
            QComboBox {
                border: 1px solid #E0E0E0;
                border-radius: 6px;
                padding: 6px 10px;
                font-size: 12px;
                background: #FFFFFF;
            }
        """)
        self.group_combo.currentIndexChanged.connect(self._on_group_combo_changed)
        self.group_combo.hide()
        header_layout.addWidget(self.group_combo)
        
        layout.addWidget(header)
        
        # 列表
        self.list_widget = QListWidget()
        self.list_widget.setStyleSheet("""
            QListWidget {
                background: transparent;
                border: none;
                outline: none;
            }
            QListWidget::item {
                padding: 10px 12px;
                border-bottom: 1px solid #F0F0F0;
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
        self.detail_title = QLabel(self.tr("New Group"))
        self.detail_title.setStyleSheet("font-size: 16px; font-weight: 600; color: #333;")
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
        
        self.delete_btn = QPushButton(self.tr("Delete"))
        self.delete_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        self.delete_btn.setStyleSheet("""
            QPushButton {
                background: #FFEBEE;
                color: #D32F2F;
                border: none;
                border-radius: 6px;
                padding: 10px 24px;
                font-size: 13px;
            }
            QPushButton:hover { background: #FFCDD2; }
        """)
        self.delete_btn.clicked.connect(self._on_delete_clicked)
        self.delete_btn.hide()
        self.btn_layout.addWidget(self.delete_btn)
        
        self.save_btn = QPushButton(self.tr("Save"))
        self.save_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        self.save_btn.setStyleSheet("""
            QPushButton {
                background: #1976D2;
                color: white;
                border: none;
                border-radius: 6px;
                padding: 10px 24px;
                font-size: 13px;
                font-weight: 500;
            }
            QPushButton:hover { background: #1565C0; }
        """)
        self.save_btn.clicked.connect(self._on_save_clicked)
        self.btn_layout.addWidget(self.save_btn)
        
        layout.addLayout(self.btn_layout)
        
        return widget
    
    def _get_nav_btn_style(self) -> str:
        return """
            QPushButton {
                background: transparent;
                color: #555;
                border: none;
                border-radius: 6px;
                padding: 10px 12px;
                font-size: 13px;
                text-align: left;
            }
            QPushButton:hover { background: rgba(0,0,0,0.05); }
            QPushButton:checked {
                background: #E3F2FD;
                color: #1976D2;
                font-weight: 500;
            }
        """
    
    def _switch_mode(self, mode: str):
        """切换模式"""
        self.current_mode = mode
        self.nav_group_btn.setChecked(mode == "group")
        self.nav_content_btn.setChecked(mode == "content")
        
        if mode == "group":
            self.list_title.setText(self.tr("Group List"))
            self.group_combo.hide()
            self._refresh_group_list()
            self._show_new_group_form()
        else:
            self.list_title.setText(self.tr("Content List"))
            self.group_combo.show()
            self._refresh_group_combo()
            self._refresh_content_list()
            self._show_new_content_form()
    
    def _refresh_group_combo(self):
        """刷新分组下拉框"""
        self.group_combo.blockSignals(True)
        self.group_combo.clear()
        
        groups = self.manager.get_groups()
        if not groups:
            self.group_combo.addItem(self.tr("(Please create a group first)"), None)
            self.selected_group_id = None
        else:
            for group in groups:
                icon = group.icon or "📁"
                self.group_combo.addItem(f"{icon} {group.name}", group.id)
            
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
        self.list_widget.clear()
        
        # 新建分组项
        new_item = QListWidgetItem(self.tr("➕ New Group"))
        new_item.setData(Qt.ItemDataRole.UserRole, ("new", None))
        self.list_widget.addItem(new_item)
        
        # 已有分组
        groups = self.manager.get_groups()
        for group in groups:
            icon = group.icon or "📁"
            item = QListWidgetItem(f"{icon} {group.name}")
            item.setData(Qt.ItemDataRole.UserRole, ("group", group.id))
            self.list_widget.addItem(item)
        
        # 默认选中第一项
        self.list_widget.setCurrentRow(0)
    
    def _refresh_content_list(self):
        """刷新内容列表（内容管理模式）"""
        self.list_widget.clear()
        
        if self.selected_group_id is None:
            item = QListWidgetItem(self.tr("(Please select a group first)"))
            item.setData(Qt.ItemDataRole.UserRole, (None, None))
            self.list_widget.addItem(item)
            return
        
        # 新建内容项
        new_item = QListWidgetItem(self.tr("➕ Add Content"))
        new_item.setData(Qt.ItemDataRole.UserRole, ("new", None))
        self.list_widget.addItem(new_item)
        
        # 分组内的内容
        items = self.manager.get_by_group(self.selected_group_id, limit=50)
        for item in items:
            # 优先显示标题，否则显示内容预览
            if item.title:
                display = item.title
            else:
                preview = item.content[:30] + "..." if len(item.content) > 30 else item.content
                display = preview.replace('\n', ' ')
            list_item = QListWidgetItem(f"📝 {display}")
            list_item.setData(Qt.ItemDataRole.UserRole, ("item", item.id))
            self.list_widget.addItem(list_item)
        
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
        self.save_btn.setText(self.tr("Create"))
        self.editing_group_id = None
        
        # 分组名称
        name_label = QLabel(self.tr("Group Name"))
        name_label.setStyleSheet("font-size: 13px; font-weight: 500; color: #555;")
        self.detail_layout.addWidget(name_label)
        
        self.group_name_input = QLineEdit()
        self.group_name_input.setPlaceholderText(self.tr("Enter group name..."))
        self.detail_layout.addWidget(self.group_name_input)
        
        # 选择图标
        icon_label = QLabel(self.tr("Select Icon"))
        icon_label.setStyleSheet("font-size: 13px; font-weight: 500; color: #555; margin-top: 8px;")
        self.detail_layout.addWidget(icon_label)
        
        # 使用网格布局，每行8个图标
        from PyQt6.QtWidgets import QGridLayout
        icon_grid = QGridLayout()
        icon_grid.setSpacing(6)
        self.icon_buttons = []
        icons_per_row = 8
        for i, icon in enumerate(self.ICONS):
            btn = QPushButton(icon)
            btn.setFixedSize(36, 36)
            btn.setCheckable(True)
            btn.setCursor(Qt.CursorShape.PointingHandCursor)
            btn.setStyleSheet("""
                QPushButton {
                    background: #F5F5F5;
                    border: 2px solid transparent;
                    border-radius: 8px;
                    font-size: 16px;
                }
                QPushButton:hover { background: #EEEEEE; }
                QPushButton:checked {
                    background: #E3F2FD;
                    border-color: #1976D2;
                }
            """)
            btn.clicked.connect(lambda checked, b=btn: self._select_icon(b))
            row = i // icons_per_row
            col = i % icons_per_row
            icon_grid.addWidget(btn, row, col)
            self.icon_buttons.append(btn)
        self.detail_layout.addLayout(icon_grid)
        
        # 默认选中第一个图标
        if self.icon_buttons:
            self.icon_buttons[0].setChecked(True)
        
        self.detail_layout.addStretch()
    
    def _show_edit_group_form(self, group_id: int):
        """显示编辑分组表单"""
        self._clear_detail_layout()
        self.detail_title.setText(self.tr("Edit Group"))
        self.delete_btn.show()
        self.save_btn.setText(self.tr("Save"))
        
        # 获取分组信息
        groups = self.manager.get_groups()
        group = next((g for g in groups if g.id == group_id), None)
        if not group:
            return
        
        # 分组名称
        name_label = QLabel(self.tr("Group Name"))
        name_label.setStyleSheet("font-size: 13px; font-weight: 500; color: #555;")
        self.detail_layout.addWidget(name_label)
        
        self.group_name_input = QLineEdit()
        self.group_name_input.setText(group.name)
        self.detail_layout.addWidget(self.group_name_input)
        
        # 选择图标
        icon_label = QLabel(self.tr("Select Icon"))
        icon_label.setStyleSheet("font-size: 13px; font-weight: 500; color: #555; margin-top: 8px;")
        self.detail_layout.addWidget(icon_label)
        
        # 使用网格布局，每行8个图标
        from PyQt6.QtWidgets import QGridLayout
        icon_grid = QGridLayout()
        icon_grid.setSpacing(6)
        self.icon_buttons = []
        current_icon = group.icon or "📁"
        icons_per_row = 8
        for i, icon in enumerate(self.ICONS):
            btn = QPushButton(icon)
            btn.setFixedSize(36, 36)
            btn.setCheckable(True)
            btn.setCursor(Qt.CursorShape.PointingHandCursor)
            btn.setStyleSheet("""
                QPushButton {
                    background: #F5F5F5;
                    border: 2px solid transparent;
                    border-radius: 8px;
                    font-size: 16px;
                }
                QPushButton:hover { background: #EEEEEE; }
                QPushButton:checked {
                    background: #E3F2FD;
                    border-color: #1976D2;
                }
            """)
            btn.clicked.connect(lambda checked, b=btn: self._select_icon(b))
            if icon == current_icon:
                btn.setChecked(True)
            row = i // icons_per_row
            col = i % icons_per_row
            icon_grid.addWidget(btn, row, col)
            self.icon_buttons.append(btn)
        self.detail_layout.addLayout(icon_grid)
        
        # 分组内容预览
        items_label = QLabel(self.tr("Group Content"))
        items_label.setStyleSheet("font-size: 13px; font-weight: 500; color: #555; margin-top: 16px;")
        self.detail_layout.addWidget(items_label)
        
        items = self.manager.get_by_group(group_id, limit=10)
        if items:
            for item in items:
                preview = item.content[:50] + "..." if len(item.content) > 50 else item.content
                item_label = QLabel(f"• {preview}")
                item_label.setStyleSheet("color: #666; font-size: 12px; padding: 4px 0;")
                item_label.setWordWrap(True)
                self.detail_layout.addWidget(item_label)
        else:
            empty_label = QLabel(self.tr("(No content in group)"))
            empty_label.setStyleSheet("color: #999; font-size: 12px;")
            self.detail_layout.addWidget(empty_label)
        
        self.detail_layout.addStretch()
    
    def _show_new_content_form(self):
        """显示新建内容表单"""
        self._clear_detail_layout()
        self.detail_title.setText(self.tr("Add Content"))
        self.delete_btn.hide()
        self.save_btn.setText(self.tr("Add"))
        self.editing_item_id = None
        
        if self.selected_group_id is None:
            hint = QLabel(self.tr("Please select a group above, or create a group first"))
            hint.setStyleSheet("color: #999; font-size: 13px;")
            self.detail_layout.addWidget(hint)
            self.detail_layout.addStretch()
            return
        
        # 标题输入
        title_label = QLabel(self.tr("Title"))
        title_label.setStyleSheet("font-size: 13px; font-weight: 500; color: #555;")
        self.detail_layout.addWidget(title_label)
        
        self.title_input = QLineEdit()
        self.title_input.setPlaceholderText(self.tr("Enter title (e.g., Restart Command)..."))
        self.detail_layout.addWidget(self.title_input)
        
        # 内容输入
        content_label = QLabel(self.tr("Content"))
        content_label.setStyleSheet("font-size: 13px; font-weight: 500; color: #555; margin-top: 8px;")
        self.detail_layout.addWidget(content_label)
        
        self.content_edit = QTextEdit()
        self.content_edit.setPlaceholderText(self.tr("Enter text content to save..."))
        self.content_edit.setMinimumHeight(180)
        self.detail_layout.addWidget(self.content_edit)
        
        self.detail_layout.addStretch()
    
    def _show_edit_content_form(self, item_id: int):
        """显示编辑内容表单"""
        self._clear_detail_layout()
        self.detail_title.setText(self.tr("Edit Content"))
        self.delete_btn.show()
        self.save_btn.setText(self.tr("Save"))
        self.editing_item_id = item_id
        
        # 获取内容
        item = self.manager.get_item(item_id)
        if not item:
            return
        
        # 标题输入
        title_label = QLabel(self.tr("Title"))
        title_label.setStyleSheet("font-size: 13px; font-weight: 500; color: #555;")
        self.detail_layout.addWidget(title_label)
        
        self.title_input = QLineEdit()
        self.title_input.setText(item.title or "")
        self.title_input.setPlaceholderText(self.tr("Enter title..."))
        self.detail_layout.addWidget(self.title_input)
        
        # 内容输入
        content_label = QLabel(self.tr("Content"))
        content_label.setStyleSheet("font-size: 13px; font-weight: 500; color: #555; margin-top: 8px;")
        self.detail_layout.addWidget(content_label)
        
        self.content_edit = QTextEdit()
        self.content_edit.setText(item.content)
        self.content_edit.setMinimumHeight(180)
        self.detail_layout.addWidget(self.content_edit)
        
        # 创建时间
        if item.created_at:
            time_label = QLabel(self.tr("Created: %s").arg(item.created_at.strftime('%Y-%m-%d %H:%M:%S')))
            time_label.setStyleSheet("color: #999; font-size: 12px; margin-top: 8px;")
            self.detail_layout.addWidget(time_label)
        
        self.detail_layout.addStretch()
    
    def _select_icon(self, btn: QPushButton):
        """选择图标"""
        for b in self.icon_buttons:
            b.setChecked(b == btn)
    
    def _get_selected_icon(self) -> str:
        """获取选中的图标"""
        for btn in self.icon_buttons:
            if btn.isChecked():
                return btn.text()
        return "📁"
    
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
            QMessageBox.warning(self, self.tr("Hint"), self.tr("Please enter group name"))
            return
        
        icon = self._get_selected_icon()
        
        if self.editing_group_id is None:
            # 新建分组
            group_id = self.manager.create_group(name, icon=icon)
            if group_id:
                self.group_added.emit()
                self._refresh_group_list()
                self.group_name_input.clear()
                self.list_widget.setCurrentRow(0)
            else:
                QMessageBox.warning(self, self.tr("Failed"), self.tr("Failed to create group"))
        else:
            # 更新分组（名称和图标）
            if self.manager.update_group(self.editing_group_id, name, icon=icon):
                self.group_added.emit()
                self._refresh_group_list()
            else:
                QMessageBox.warning(self, self.tr("Failed"), self.tr("Failed to update group"))
    
    def _save_content(self):
        """保存内容"""
        if self.selected_group_id is None:
            QMessageBox.warning(self, self.tr("Hint"), self.tr("Please select a group first"))
            return
        
        content = self.content_edit.toPlainText().strip()
        if not content:
            QMessageBox.warning(self, self.tr("Hint"), self.tr("Please enter content"))
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
                    self._refresh_content_list()
                    self.list_widget.setCurrentRow(0)
                else:
                    QMessageBox.warning(self, self.tr("Failed"), self.tr("Failed to move to group"))
            else:
                QMessageBox.warning(self, self.tr("Failed"), self.tr("Failed to add content"))
        else:
            # 编辑内容（使用 update_item）
            if self.manager.update_item(self.editing_item_id, content, title=title):
                self.content_added.emit(self.selected_group_id)
                self._refresh_content_list()
            else:
                QMessageBox.warning(self, self.tr("Failed"), self.tr("Failed to update content"))
    
    def _on_delete_clicked(self):
        """删除按钮点击"""
        if self.current_mode == "group" and self.editing_group_id:
            reply = QMessageBox.question(
                self, self.tr("Confirm Delete"),
                self.tr("Are you sure you want to delete this group?\nAll items in the group will also be deleted."),
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
                QMessageBox.StandardButton.No
            )
            if reply == QMessageBox.StandardButton.Yes:
                if self.manager.delete_group(self.editing_group_id):
                    self.group_added.emit()
                    self._refresh_group_list()
                    self._show_new_group_form()
                else:
                    QMessageBox.warning(self, self.tr("Failed"), self.tr("Failed to delete group"))
        
        elif self.current_mode == "content" and self.editing_item_id:
            reply = QMessageBox.question(
                self, self.tr("Confirm Delete"),
                self.tr("Are you sure you want to delete this item?"),
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
                QMessageBox.StandardButton.No
            )
            if reply == QMessageBox.StandardButton.Yes:
                if self.manager.delete_item(self.editing_item_id):
                    self.content_added.emit(self.selected_group_id)
                    self._refresh_content_list()
                    self._show_new_content_form()
                else:
                    QMessageBox.warning(self, self.tr("Failed"), self.tr("Failed to delete item"))
    
    def _switch_page(self, index: int):
        """切换页面（兼容旧接口）"""
        if index == 0:
            self._switch_mode("group")
        else:
            self._switch_mode("content")


if __name__ == "__main__":
    import sys
    app = QApplication(sys.argv)
    manager = ClipboardManager()
    dialog = ManageDialog(manager)
    dialog.show()
    sys.exit(app.exec())