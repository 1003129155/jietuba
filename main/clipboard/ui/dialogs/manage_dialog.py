# -*- coding: utf-8 -*-
"""
剪贴板管理窗口 - 三列布局（独立窗口）

第1列：导航（分组管理、内容管理）
第2列：列表（分组列表/内容分组选择）
第3列：详细编辑区

这是一个独立的设置窗口，不是剪贴板窗口的子窗口。
使用单例模式确保只有一个实例存在。
"""

from typing import Optional

from PySide6.QtCore import Qt, Signal
from PySide6.QtGui import QColor, QCursor
from PySide6.QtWidgets import (
    QApplication,
    QFileDialog,
    QFrame,
    QHBoxLayout,
    QListWidget,
    QListWidgetItem,
    QPushButton,
    QScrollArea,
    QVBoxLayout,
    QWidget,
)
from shiboken6 import isValid

from ui.fluent_lite import (
    BodyLabel,
    CaptionLabel,
    ComboBox,
    FluentIcon,
    NavigationInterface,
    NavigationItemPosition,
    PushButton as FluentPushButton,
    PrimaryPushButton,
    FluentTitleBar,
    FrostedFramelessDialog,
)
from core import safe_event
from core.ui_theme import get_ui_theme
from ui.dialogs import show_confirm_dialog, show_info_dialog, show_warning_dialog

from ...core import ClipboardManager, Group, GroupType
from ...services.file_payload_service import extract_first_file_path_from_content
from ...services.group_service import (
    build_delete_group_confirm_message,
    get_group_display_icon,
    get_toggled_default_group_icon,
    group_name_exists,
    make_unique_group_name,
)
from ...services.import_export_service import (
    collect_text_export_rows,
    import_text_rows,
    read_import_rows,
    write_csv_rows,
)
from ...services.manage_dialog_service import save_file_content, save_group, save_text_content
from ..forms.file_content_form import build_edit_file_content_form, build_file_content_form
from ..forms.group_form import build_edit_group_form, build_new_group_form
from ..forms.group_icon_picker import (
    create_group_icon_picker,
    emoji_btn_style,
    emoji_tab_style,
    on_icon_input_changed,
    on_preset_icon_clicked,
    switch_emoji_group,
)
from ..forms.import_export_form import build_import_export_form
from ..forms.text_content_form import build_edit_text_content_form, build_text_content_form
from ..layout_scale import (
    MANAGE_DIALOG_HEIGHT,
    MANAGE_DIALOG_MIN_HEIGHT,
    MANAGE_DIALOG_MIN_WIDTH,
    MANAGE_DIALOG_WIDTH,
    scale_ui,
    scale_x,
    scale_y,
)
from ..widgets.draggable_list_widget import DraggableListWidget


_manage_window_instance: Optional["ManageDialog"] = None


def _qt_object_is_valid(obj) -> bool:
    if obj is None:
        return False
    try:
        return isValid(obj)
    except RuntimeError:
        return False


def get_manage_dialog(manager: ClipboardManager = None) -> "ManageDialog":
    """获取管理窗口的单例实例"""
    global _manage_window_instance
    if _manage_window_instance is None:
        if manager is None:
            manager = ClipboardManager()
        _manage_window_instance = ManageDialog(manager)
    return _manage_window_instance


def get_existing_manage_dialog() -> Optional["ManageDialog"]:
    """获取已存在的管理窗口实例，不主动创建。"""
    return _manage_window_instance


class ManageDialog(FrostedFramelessDialog):
    """剪贴板管理窗口 - 三列布局（独立窗口）"""

    group_added = Signal()
    content_added = Signal(int)
    data_changed = Signal()

    def __init__(self, manager: ClipboardManager, parent=None):
        super().__init__(parent)
        self.manager = manager
        self.current_mode = "group"
        self.selected_group_id = None
        self.editing_group_id = None
        self.editing_item_id = None
        self._detail_form_token = 0

        self._setup_titlebar()

        self.setWindowTitle(self.tr("Clipboard Management"))
        self.setMinimumSize(MANAGE_DIALOG_MIN_WIDTH, MANAGE_DIALOG_MIN_HEIGHT)
        self.resize(MANAGE_DIALOG_WIDTH, MANAGE_DIALOG_HEIGHT)
        self.setWindowFlags(
            self.windowFlags()
            | Qt.WindowType.Window
            | Qt.WindowType.WindowCloseButtonHint
            | Qt.WindowType.WindowMinimizeButtonHint
            | Qt.WindowType.WindowMaximizeButtonHint
        )
        try:
            from core.resource_manager import ResourceManager
            import os

            icon_path = ResourceManager.get_resource_path("svg/托盘.svg")
            if os.path.exists(icon_path):
                from PySide6.QtGui import QIcon

                self.setWindowIcon(QIcon(icon_path))
        except Exception:
            pass
        self.setAttribute(Qt.WidgetAttribute.WA_DeleteOnClose, False)

        self._setup_ui()
        self._switch_mode("group")
        self._center_on_screen()
        self._apply_ui_theme(get_ui_theme().tokens)
        get_ui_theme().theme_changed.connect(self._apply_ui_theme)

    def _refresh_theme_scope(self, tokens=None):
        """Reapply Fluent styles after controls have acquired this parent."""
        tokens = tokens or get_ui_theme().tokens
        for widget in self.findChildren(QWidget):
            apply_theme = getattr(widget, "_apply_theme", None)
            if callable(apply_theme):
                apply_theme(tokens)
            else:
                widget.update()

    def _apply_ui_theme(self, tokens):
        """Apply the same application theme used by the settings window."""
        self._refresh_theme_scope(tokens)
        error_background = "#4A2328" if tokens.is_dark else "#FFEBEE"
        error_text = "#FF8A80" if tokens.is_dark else "#D32F2F"
        error_hover = "#603036" if tokens.is_dark else "#FFCDD2"
        self.setStyleSheet(f"""
            QTextEdit {{
                border: 1px solid {tokens.border};
                border-radius: {scale_ui(6)}px;
                padding: {scale_ui(8)}px;
                font-size: {scale_ui(13)}px;
                background: {tokens.input_background};
                color: {tokens.text};
                selection-background-color: {tokens.accent};
            }}
            QTextEdit:focus {{
                border-color: {tokens.accent};
                background: {tokens.surface_strong};
            }}
            QScrollBar:vertical {{
                width: {scale_x(7)}px;
                background: transparent;
            }}
            QScrollBar::handle:vertical {{
                min-height: {scale_y(24)}px;
                background: {tokens.border_hover};
                border-radius: {scale_ui(3)}px;
            }}
            QScrollBar::handle:vertical:hover {{
                background: {tokens.accent};
            }}
            QScrollBar::add-line:vertical,
            QScrollBar::sub-line:vertical {{
                height: 0;
            }}
            QScrollBar:horizontal {{
                height: {scale_y(7)}px;
                background: transparent;
            }}
            QScrollBar::handle:horizontal {{
                min-width: {scale_x(24)}px;
                background: {tokens.border_hover};
                border-radius: {scale_ui(3)}px;
            }}
            QScrollBar::handle:horizontal:hover {{
                background: {tokens.accent};
            }}
            QScrollBar::add-line:horizontal,
            QScrollBar::sub-line:horizontal {{
                width: 0;
            }}
        """)

        self.nav_column.setStyleSheet(
            "QWidget#ManageNavColumn {"
            f"background: {tokens.surface_subtle};"
            f"border-right: 1px solid {tokens.separator};"
            f"border-radius: {scale_ui(12)}px 0 0 {scale_ui(12)}px;"
            "}"
        )
        self.list_column.setStyleSheet(
            "QWidget#ManageListColumn {"
            f"background: {tokens.surface};"
            f"border-right: 1px solid {tokens.separator};"
            "}"
        )
        self.detail_column.setStyleSheet(
            "QWidget#ManageDetailColumn {"
            f"background: {tokens.surface_strong};"
            f"border-radius: 0 {scale_ui(12)}px {scale_ui(12)}px 0;"
            "}"
        )
        self.list_widget.setStyleSheet(f"""
            QListWidget {{
                background: transparent;
                border: none;
                outline: none;
                color: {tokens.text};
            }}
            QListWidget::item {{
                padding: 0px {scale_x(12)}px;
                border-bottom: 1px solid {tokens.separator};
                color: {tokens.text};
            }}
            QListWidget::item:selected {{
                background: {tokens.accent_soft};
                color: {tokens.text};
            }}
            QListWidget::item:hover {{
                background: {tokens.surface_hover};
            }}
        """)
        self.detail_title.setStyleSheet(
            f"color: {tokens.text}; font-size: {scale_ui(16)}px; "
            "font-weight: 600; background: transparent;"
        )
        self.detail_subtitle.setStyleSheet(
            f"color: {tokens.text_muted}; font-size: {scale_ui(12)}px; "
            "background: transparent;"
        )
        self.detail_separator.setStyleSheet(
            f"background: {tokens.separator}; border: none;"
        )
        self.delete_btn.setStyleSheet(f"""
            QPushButton {{
                background: {error_background};
                color: {error_text};
                border: none;
                border-radius: {scale_ui(6)}px;
                padding: {scale_y(10)}px {scale_x(24)}px;
                font-size: {scale_ui(13)}px;
            }}
            QPushButton:hover {{ background: {error_hover}; }}
        """)
        for row in range(self.list_widget.count()):
            item = self.list_widget.item(row)
            data = item.data(Qt.ItemDataRole.UserRole)
            if data and data[0] == "new" and self.current_mode == "content":
                item.setBackground(QColor(tokens.accent_soft))
        self._apply_dynamic_form_theme()
        self.update()

    def _apply_dynamic_form_theme(self):
        """Refresh controls that are rebuilt when the selected form changes."""
        tokens = get_ui_theme().tokens
        icon_input = getattr(self, "icon_input", None)
        if _qt_object_is_valid(icon_input):
            apply_theme = getattr(icon_input, "_apply_theme", None)
            if callable(apply_theme):
                apply_theme(tokens)
            icon_input.setStyleSheet(
                icon_input.styleSheet()
                + f"QLineEdit {{ font-size: {scale_ui(18)}px; "
                  f"min-width: {scale_x(120)}px; max-width: {scale_x(150)}px; }}"
            )
        icon_preview = getattr(self, "icon_preview", None)
        if _qt_object_is_valid(icon_preview):
            icon_preview.setStyleSheet(f"""
                QLabel {{
                    font-size: {scale_ui(28)}px;
                    background: {tokens.surface_subtle};
                    border: 2px solid {tokens.border};
                    border-radius: {scale_ui(8)}px;
                }}
            """)
        for index, button in enumerate(
            getattr(self, "_emoji_tab_buttons", [])
        ):
            button.setStyleSheet(
                emoji_tab_style(
                    index == getattr(self, "_emoji_current_idx", 0),
                    self,
                )
            )
        emoji_scroll = getattr(self, "_emoji_scroll", None)
        if _qt_object_is_valid(emoji_scroll):
            emoji_scroll.setStyleSheet(f"""
                QScrollArea {{ border: none; background: transparent; }}
                QScrollBar:vertical {{
                    width: {scale_x(6)}px; background: transparent;
                }}
                QScrollBar::handle:vertical {{
                    background: {tokens.border_hover};
                    border-radius: {scale_ui(3)}px;
                    min-height: {scale_y(20)}px;
                }}
                QScrollBar::add-line:vertical,
                QScrollBar::sub-line:vertical {{ height: 0px; }}
            """)
        import_export_separator = getattr(
            self, "import_export_separator", None
        )
        if _qt_object_is_valid(import_export_separator):
            import_export_separator.setStyleSheet(
                f"background: {tokens.separator}; "
                f"margin: {scale_y(16)}px 0;"
            )

    def _setup_titlebar(self):
        title_bar = FluentTitleBar(self)
        self.setTitleBar(title_bar)
        title_bar.setDoubleClickEnabled(True)

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
        event.ignore()

    def _setup_ui(self):
        """设置三列布局"""
        main_layout = QHBoxLayout(self)
        title_height = self.titleBar.height() if getattr(self, "titleBar", None) else 32
        main_layout.setContentsMargins(scale_x(8), title_height + scale_y(4), scale_x(8), scale_y(8))
        main_layout.setSpacing(0)

        self.nav_column = self._create_nav_column()
        main_layout.addWidget(self.nav_column)

        self.list_column = self._create_list_column()
        main_layout.addWidget(self.list_column)

        self.detail_column = self._create_detail_column()
        main_layout.addWidget(self.detail_column, 1)

    def _create_nav_column(self) -> QWidget:
        """创建导航列（使用 Fluent NavigationInterface）"""
        widget = QWidget()
        widget.setObjectName("ManageNavColumn")
        widget.setFixedWidth(scale_x(180))

        layout = QVBoxLayout(widget)
        layout.setContentsMargins(0, scale_y(8), 0, scale_y(8))
        layout.setSpacing(scale_y(4))

        title = BodyLabel(self.tr("Management"))
        self.nav_title = title
        title.setStyleSheet(
            f"font-size: {scale_ui(14)}px; font-weight: 600; "
            f"padding: {scale_y(8)}px {scale_x(12)}px {scale_y(8)}px {scale_x(12)}px; "
            "background: transparent;"
        )
        layout.addWidget(title)

        self.nav_interface = NavigationInterface(parent=widget, showMenuButton=False, showReturnButton=False, collapsible=False)
        self.nav_interface.setExpandWidth(scale_x(168))
        self.nav_interface.setMinimumExpandWidth(0)
        self.nav_interface.expand(useAni=False)
        self.nav_interface.setMinimumWidth(scale_x(168))
        self.nav_interface.setMaximumWidth(scale_x(176))

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

        close_btn = FluentPushButton(self.tr("Close"))
        self.close_btn = close_btn
        close_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        close_btn.clicked.connect(self.close)
        close_btn_layout = QHBoxLayout()
        close_btn_layout.setContentsMargins(scale_x(8), 0, scale_x(8), 0)
        close_btn_layout.addWidget(close_btn)
        layout.addLayout(close_btn_layout)

        return widget

    def _create_list_column(self) -> QWidget:
        """创建列表列"""
        widget = QWidget()
        widget.setObjectName("ManageListColumn")
        widget.setFixedWidth(scale_x(220))

        layout = QVBoxLayout(widget)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)

        header = QWidget()
        header.setStyleSheet("background: transparent;")
        header_layout = QVBoxLayout(header)
        header_layout.setContentsMargins(scale_x(12), scale_y(12), scale_x(12), scale_y(8))
        header_layout.setSpacing(scale_y(8))

        self.list_title = BodyLabel(self.tr("Group List"))
        self.list_title.setStyleSheet(f"font-size: {scale_ui(13)}px; font-weight: 500; background: transparent;")
        header_layout.addWidget(self.list_title)

        self.group_combo = ComboBox()
        self.group_combo.currentIndexChanged.connect(self._on_group_combo_changed)
        self.group_combo.hide()
        header_layout.addWidget(self.group_combo)

        layout.addWidget(header)

        self.list_widget = DraggableListWidget()
        self.list_widget.setDragDropMode(QListWidget.DragDropMode.InternalMove)
        self.list_widget.setDefaultDropAction(Qt.DropAction.MoveAction)
        self.list_widget.setSelectionMode(QListWidget.SelectionMode.SingleSelection)
        self.list_widget.itemClicked.connect(self._on_list_item_clicked)
        self.list_widget.model().rowsMoved.connect(self._on_list_reordered)
        layout.addWidget(self.list_widget, 1)

        return widget

    def _create_detail_column(self) -> QWidget:
        """创建详情列"""
        widget = QWidget()
        widget.setObjectName("ManageDetailColumn")

        layout = QVBoxLayout(widget)
        layout.setContentsMargins(scale_x(24), scale_y(20), scale_x(24), scale_y(20))
        layout.setSpacing(scale_y(16))

        self.detail_title = BodyLabel(self.tr("New Group"))
        self.detail_title.setStyleSheet(f"font-size: {scale_ui(16)}px; font-weight: 600;")
        layout.addWidget(self.detail_title)

        self.detail_subtitle = CaptionLabel("")
        self.detail_subtitle.setStyleSheet(f"font-size: {scale_ui(12)}px;")
        self.detail_subtitle.setWordWrap(True)
        self.detail_subtitle.hide()
        layout.addWidget(self.detail_subtitle)

        line = QFrame()
        self.detail_separator = line
        line.setFrameShape(QFrame.Shape.HLine)
        line.setFixedHeight(1)
        layout.addWidget(line)

        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QFrame.Shape.NoFrame)
        scroll.setStyleSheet("QScrollArea { background: transparent; border: none; }")

        self.detail_content = QWidget()
        self.detail_content.setStyleSheet("background: transparent;")
        self.detail_layout = QVBoxLayout(self.detail_content)
        self.detail_layout.setContentsMargins(0, 0, 0, 0)
        self.detail_layout.setSpacing(scale_y(16))

        scroll.setWidget(self.detail_content)
        layout.addWidget(scroll, 1)

        self.btn_layout = QHBoxLayout()
        self.btn_layout.setSpacing(scale_x(12))
        self.btn_layout.addStretch()

        self.delete_btn = FluentPushButton(self.tr("Delete"))
        self.delete_btn.setCursor(Qt.CursorShape.PointingHandCursor)
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
            self.list_column.show()
            self.list_title.setText(self.tr("Group List"))
            self.group_combo.hide()
            self.list_widget.show()
            self._refresh_group_list()
            self._show_new_group_form()
        elif mode == "content":
            self.list_column.show()
            self.list_title.setText(self.tr("Content List"))
            self.group_combo.show()
            self.list_widget.show()
            self._refresh_group_combo()
            self._refresh_content_list()
            self._show_new_content_form()
        elif mode == "import_export":
            self.list_column.hide()
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
                icon = get_group_display_icon(group.icon, group.group_type == GroupType.FILE, is_hidden=(group.group_type == GroupType.HIDDEN))
                self.group_combo.addItem(f"{icon} {group.name}", userData=group.id)

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
        current_item = self.list_widget.currentItem()
        saved_group_id = None
        if current_item:
            data = current_item.data(Qt.ItemDataRole.UserRole)
            if data and data[0] == "group":
                saved_group_id = data[1]

        self.list_widget.clear()

        new_item = QListWidgetItem(self.tr("New Group"))
        new_item.setData(Qt.ItemDataRole.UserRole, ("new", None))
        self.list_widget.addItem(new_item)

        groups = self.manager.get_groups()
        restored_selection = False
        for i, group in enumerate(groups):
            icon = get_group_display_icon(group.icon, group.group_type == GroupType.FILE, is_hidden=(group.group_type == GroupType.HIDDEN))
            item = QListWidgetItem(f"{icon} {group.name}")
            item.setData(Qt.ItemDataRole.UserRole, ("group", group.id))
            self.list_widget.addItem(item)

            if saved_group_id is not None and group.id == saved_group_id:
                self.list_widget.setCurrentRow(i + 1)
                restored_selection = True

        if not restored_selection:
            self.list_widget.setCurrentRow(0)

    def _refresh_content_list(self):
        """刷新内容列表（内容管理模式）"""
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

        new_item = QListWidgetItem(self.tr("Add Content"))
        new_item.setData(Qt.ItemDataRole.UserRole, ("new", None))
        new_item.setBackground(QColor(get_ui_theme().tokens.accent_soft))
        self.list_widget.addItem(new_item)

        items = self.manager.get_by_group(self.selected_group_id, limit=50)
        restored_selection = False
        for i, item in enumerate(items):
            if item.title:
                display = item.title
            else:
                preview = item.content[:30] + "..." if len(item.content) > 30 else item.content
                display = preview.replace("\n", " ")
            list_item = QListWidgetItem(f"{display}")
            list_item.setData(Qt.ItemDataRole.UserRole, ("item", item.id))
            self.list_widget.addItem(list_item)

            if saved_item_id is not None and item.id == saved_item_id:
                self.list_widget.setCurrentRow(i + 1)
                restored_selection = True

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
        if self.current_mode == "group":
            self._handle_group_reorder(start, row)
        else:
            self._handle_item_reorder(start, row)

    def _handle_group_reorder(self, old_index: int, new_index: int):
        """处理分组拖拽排序"""
        from core.logger import T, log_debug, log_info, log_error, log_exception

        log_debug(T("old_index={old_index}, new_index={new_index}", old_index=old_index, new_index=new_index), "GroupDrag")

        groups = self.manager.get_groups()

        log_debug(T("当前有 {count} 个分组", count=len(groups)), "GroupDrag")

        if old_index <= 0 or old_index > len(groups):
            log_error(T("索引越界: old_index={old_index}", old_index=old_index), "GroupDrag")
            return

        old_pos = old_index - 1
        new_pos = new_index - 1 if new_index > 0 else 0

        if old_pos < 0 or old_pos >= len(groups):
            log_error(T("调整后索引越界: old_pos={old_pos}", old_pos=old_pos), "GroupDrag")
            return

        moved_group = groups[old_pos]
        log_debug(T("移动分组: ID={group_id}, name={group_name}", group_id=moved_group.id, group_name=moved_group.name), "GroupDrag")

        temp_groups = [g for i, g in enumerate(groups) if i != old_pos]
        adjusted_new_pos = new_pos if new_pos < old_pos else new_pos - 1

        before_id = None
        after_id = None

        if adjusted_new_pos > 0:
            before_id = temp_groups[adjusted_new_pos - 1].id

        if adjusted_new_pos < len(temp_groups):
            after_id = temp_groups[adjusted_new_pos].id

        log_debug(T("计算: before_id={before_id}, after_id={after_id}", before_id=before_id, after_id=after_id), "GroupDrag")

        try:
            self.manager._manager.move_group_between(moved_group.id, before_id=before_id, after_id=after_id)
            log_info(T("移动成功: ID={group_id}, before={before_id}, after={after_id}", group_id=moved_group.id, before_id=before_id, after_id=after_id), "GroupDrag")
            self._refresh_group_list()
            self.group_added.emit()
        except Exception as e:
            log_error(T("移动失败: {e}", e=e), "GroupDrag")
            log_exception(e, T("拖拽分组移动失败"))
            self._refresh_group_list()

    def _handle_item_reorder(self, old_index: int, new_index: int):
        """处理内容拖拽排序"""
        from core.logger import T, log_debug, log_info, log_error, log_exception

        log_debug(T("old_index={old_index}, new_index={new_index}", old_index=old_index, new_index=new_index), "ItemDrag")

        if self.selected_group_id is None:
            log_error(T("未选择分组"), "ItemDrag")
            return

        items = self.manager.get_by_group(self.selected_group_id, offset=0, limit=1000)

        log_debug(T("当前分组有 {count} 个内容", count=len(items)), "ItemDrag")

        if old_index <= 0 or old_index > len(items):
            log_error(T("索引越界: old_index={old_index}, items count={items_count}", old_index=old_index, items_count=len(items)), "ItemDrag")
            return

        old_pos = old_index - 1
        new_pos = new_index - 1 if new_index > 0 else 0

        if old_pos < 0 or old_pos >= len(items):
            log_error(T("调整后索引越界: old_pos={old_pos}", old_pos=old_pos), "ItemDrag")
            return

        moved_item = items[old_pos]
        log_debug(T("移动项: ID={item_id}, title={item_title}", item_id=moved_item.id, item_title=moved_item.title or moved_item.content[:20]), "ItemDrag")

        temp_items = [item for i, item in enumerate(items) if i != old_pos]
        adjusted_new_pos = new_pos if new_pos < old_pos else new_pos - 1

        before_id = None
        after_id = None

        if adjusted_new_pos > 0:
            before_id = temp_items[adjusted_new_pos - 1].id

        if adjusted_new_pos < len(temp_items):
            after_id = temp_items[adjusted_new_pos].id

        log_debug(T("计算: before_id={before_id}, after_id={after_id}", before_id=before_id, after_id=after_id), "ItemDrag")

        try:
            self.manager._manager.move_item_between(moved_item.id, before_id=before_id, after_id=after_id)
            log_info(T("移动成功: ID={item_id}, before={before_id}, after={after_id}", item_id=moved_item.id, before_id=before_id, after_id=after_id), "ItemDrag")
            self._refresh_content_list()
        except Exception as e:
            log_error(T("移动失败: {e}", e=e), "ItemDrag")
            log_exception(e, T("拖拽内容移动失败"))
            self._refresh_content_list()

    def _clear_detail_layout(self):
        """清空详情区域"""
        self._detail_form_token += 1
        self.icon_buttons = []
        for attr in (
            "group_name_input",
            "icon_input",
            "icon_preview",
            "radio_normal",
            "radio_file",
            "radio_hidden",
            "_group_type_btn_group",
            "_emoji_scroll",
        ):
            widget = getattr(self, attr, None)
            if _qt_object_is_valid(widget):
                try:
                    widget.blockSignals(True)
                except Exception:
                    pass
            setattr(self, attr, None)
        self._emoji_tab_buttons = []
        self._emoji_group_order = []
        self._emoji_groups = {}

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

    def _is_current_detail_form_token(self, form_token: Optional[int]) -> bool:
        return form_token is None or form_token == self._detail_form_token

    def _show_new_group_form(self):
        """显示新建分组表单"""
        self._clear_detail_layout()
        self.delete_btn.hide()
        self.save_btn.show()
        self.save_btn.setText(self.tr("Create"))
        self.editing_group_id = None

        build_new_group_form(self)
        self._update_group_form_header()
        self._refresh_theme_scope()
        self._apply_dynamic_form_theme()

    def _on_group_type_toggled(self, checked: bool, form_token: Optional[int] = None):
        """分组类型切换时，若用户未手动修改图标则自动切换默认图标"""
        if not self._is_current_detail_form_token(form_token):
            return
        if not _qt_object_is_valid(getattr(self, "icon_input", None)):
            return
        if not checked:
            return  # 只处理选中事件，避免重复触发
        current = self.icon_input.text()
        is_file_group = _qt_object_is_valid(getattr(self, "radio_file", None)) and self.radio_file.isChecked()
        is_hidden = _qt_object_is_valid(getattr(self, "radio_hidden", None)) and self.radio_hidden.isChecked()
        self.icon_input.setText(get_toggled_default_group_icon(current, is_file_group, is_hidden=is_hidden))
        self._update_group_form_header()

    def _set_detail_subtitle(self, text: str):
        """设置详情区标题下方的小字说明。"""
        self.detail_subtitle.setText(text)
        self.detail_subtitle.setVisible(bool(text))

    def _update_group_form_header(self):
        """根据当前分组类型刷新分组表单标题和说明。"""
        if not _qt_object_is_valid(getattr(self, "radio_file", None)):
            return

        is_file_group = self.radio_file.isChecked()
        is_hidden = _qt_object_is_valid(getattr(self, "radio_hidden", None)) and self.radio_hidden.isChecked()

        if is_hidden:
            if self.editing_group_id is None:
                self.detail_title.setText(self.tr("New Hidden Group"))
            else:
                self.detail_title.setText(self.tr("Edit Hidden Group"))
            subtitle = self.tr("Hidden groups are not displayed on the clipboard panel; changing to normal group restores visibility")
        elif is_file_group:
            if self.editing_group_id is None:
                self.detail_title.setText(self.tr("New Quick Launch Group"))
            else:
                self.detail_title.setText(self.tr("Edit Quick Launch Group"))
            subtitle = self.tr("Quick launch groups only allow file or folder paths; selecting an item opens them")
        else:
            if self.editing_group_id is None:
                self.detail_title.setText(self.tr("New General Group"))
            else:
                self.detail_title.setText(self.tr("Edit General Group"))
            subtitle = self.tr("General groups allow any content; selecting an item pastes content or files to the target")
        self._set_detail_subtitle(subtitle)

    def _create_emoji_picker(self, current_icon: str = "📁"):
        """创建 emoji 选择器（输入框 + 预览 + 分组标签页 + 可滚动网格）"""
        create_group_icon_picker(self, current_icon)

    def _emoji_tab_style(self, active: bool) -> str:
        """返回 emoji 分组 tab 按钮样式"""
        return emoji_tab_style(active, self)

    def _emoji_btn_style(self) -> str:
        """返回 emoji 网格按钮样式"""
        return emoji_btn_style(self)

    def _switch_emoji_group(self, group_idx: int, form_token: Optional[int] = None):
        """切换 emoji 分组"""
        if not self._is_current_detail_form_token(form_token):
            return
        if not _qt_object_is_valid(getattr(self, "_emoji_scroll", None)):
            return
        if not getattr(self, "_emoji_tab_buttons", None):
            return
        switch_emoji_group(self, group_idx)

    def _on_icon_input_changed(self, text: str, form_token: Optional[int] = None):
        """输入框内容变化时更新预览（只保留第一个 emoji）"""
        if not self._is_current_detail_form_token(form_token):
            return
        if not _qt_object_is_valid(getattr(self, "icon_input", None)):
            return
        if not _qt_object_is_valid(getattr(self, "icon_preview", None)):
            return
        on_icon_input_changed(self, text)

    def _on_preset_icon_clicked(self, icon: str, form_token: Optional[int] = None):
        """点击预设图标"""
        if not self._is_current_detail_form_token(form_token):
            return
        if not _qt_object_is_valid(getattr(self, "icon_input", None)):
            return
        if not _qt_object_is_valid(getattr(self, "icon_preview", None)):
            return
        on_preset_icon_clicked(self, icon)

    def _show_edit_group_form(self, group_id: int):
        """显示编辑分组表单"""
        self._clear_detail_layout()
        self.delete_btn.show()
        self.save_btn.show()
        self.save_btn.setText(self.tr("Save"))
        self.editing_group_id = group_id

        groups = self.manager.get_groups()
        group = next((g for g in groups if g.id == group_id), None)
        if not group:
            return

        build_edit_group_form(self, group)
        self._update_group_form_header()
        self._refresh_theme_scope()
        self._apply_dynamic_form_theme()

    def _get_selected_group(self) -> Optional[Group]:
        """获取当前选中分组对象"""
        if self.selected_group_id is None:
            return None
        groups = self.manager.get_groups()
        return next((g for g in groups if g.id == self.selected_group_id), None)

    def _is_file_content_mode(self) -> bool:
        """判断当前内容编辑是否应走文件条目表单。
        隐藏分组的行为与普通分组相同，不走文件表单。"""
        if self.editing_item_id is not None:
            item = self.manager.get_item(self.editing_item_id)
            if item is not None:
                return item.content_type == "file"

        selected_group = self._get_selected_group()
        if selected_group is None:
            return False
        # 只有快速启动分组才走文件表单；隐藏分组和普通分组一样走文本表单
        return selected_group.group_type == GroupType.FILE

    def _extract_file_path_from_item(self, item) -> str:
        """从文件条目内容中提取首个路径，兼容旧格式原始路径文本。"""
        if item is None or item.content_type != "file":
            return ""
        return extract_first_file_path_from_content(item.content)

    def _show_new_content_form(self):
        """显示新建内容表单"""
        self._clear_detail_layout()
        self.detail_title.setText(self.tr("Add Content"))
        self._set_detail_subtitle("")
        self.delete_btn.hide()
        self.save_btn.show()
        self.save_btn.setText(self.tr("Add"))
        self.editing_item_id = None

        if self.selected_group_id is None:
            hint = CaptionLabel(self.tr("Please select a group above, or create a group first"))
            self.detail_layout.addWidget(hint)
            self.detail_layout.addStretch()
            self._refresh_theme_scope()
            return

        selected_group = self._get_selected_group()
        if selected_group is not None and selected_group.group_type == GroupType.FILE:
            self._build_file_content_form()
        else:
            self._build_text_content_form()
        self._refresh_theme_scope()
        self._apply_dynamic_form_theme()

    def _build_text_content_form(self):
        """普通分组 — 文本内容输入表单"""
        build_text_content_form(self)

    def _build_file_content_form(self, add_stretch: bool = True):
        """文件分组 — 文件选择表单"""
        build_file_content_form(self, add_stretch=add_stretch)

    def _set_selected_file_path(self, path: str):
        """统一设置当前选中的文件或文件夹路径。"""
        import os

        normalized_path = os.path.normpath(path)
        self.selected_file_path = normalized_path
        if hasattr(self, "file_path_input"):
            self.file_path_input.setText(normalized_path)

    def _on_browse_file(self):
        """弹出文件选择对话框"""
        path, _ = QFileDialog.getOpenFileName(self, self.tr("Select File"), "", self.tr("All Files (*.*)"))
        if path:
            self._set_selected_file_path(path)

    def _on_browse_folder(self):
        """弹出文件夹选择对话框"""
        path = QFileDialog.getExistingDirectory(self, self.tr("Select Folder"), "")
        if path:
            self._set_selected_file_path(path)

    def _show_edit_content_form(self, item_id: int):
        """显示编辑内容表单"""
        self._clear_detail_layout()
        self.detail_title.setText(self.tr("Edit Content"))
        self._set_detail_subtitle("")
        self.delete_btn.show()
        self.save_btn.show()
        self.save_btn.setText(self.tr("Save"))
        self.editing_item_id = item_id

        item = self.manager.get_item(item_id)
        if not item:
            return

        if item.content_type == "file":
            file_path = self._extract_file_path_from_item(item)
            build_edit_file_content_form(self, item, file_path)
        else:
            build_edit_text_content_form(self, item)

        if item.created_at:
            time_str = item.created_at.strftime("%Y-%m-%d %H:%M:%S")
            time_label = CaptionLabel(f"{self.tr('Created')}: {time_str}")
            self.detail_layout.addWidget(time_label)

        if item.content_type == "file":
            self.detail_layout.addStretch()
        self._refresh_theme_scope()
        self._apply_dynamic_form_theme()

    def _show_import_export_form(self):
        """显示导入导出表单"""
        self._clear_detail_layout()
        self.detail_title.setText(self.tr("Import/Export"))
        self._set_detail_subtitle("")
        self.delete_btn.hide()
        self.save_btn.hide()

        build_import_export_form(self)
        self._refresh_theme_scope()
        self._apply_dynamic_form_theme()

    def _select_icon(self, btn: QPushButton):
        """选择图标（旧方法，保留兼容）"""
        for b in self.icon_buttons:
            b.setChecked(b == btn)

    def _get_selected_icon(self) -> str:
        """获取选中的图标（优先从输入框获取，确保只有一个字符）"""
        if _qt_object_is_valid(getattr(self, "icon_input", None)):
            text = self.icon_input.text().strip()
            if text:
                for char in text:
                    if not char.isspace():
                        return char
        for btn in self.icon_buttons:
            if btn.isChecked():
                return btn.text()
        return "📁"

    def _get_delete_group_confirm_message(self) -> str:
        return build_delete_group_confirm_message(self.tr)

    def _group_name_exists(self, name: str, exclude_group_id: Optional[int] = None) -> bool:
        return group_name_exists(self.manager.get_groups(), name, exclude_group_id=exclude_group_id)

    @staticmethod
    def _make_unique_group_name(base_name: str, used_names: set[str]) -> str:
        return make_unique_group_name(base_name, used_names)

    def _on_save_clicked(self):
        """保存按钮点击"""
        if self.current_mode == "group":
            self._save_group()
        else:
            self._save_content()

    def _save_group(self):
        """保存分组"""
        if not _qt_object_is_valid(getattr(self, "group_name_input", None)):
            return
        name = self.group_name_input.text().strip()
        if not name:
            show_warning_dialog(self, self.tr("Hint"), self.tr("Please enter group name"))
            return

        if self._group_name_exists(name, exclude_group_id=self.editing_group_id):
            show_warning_dialog(self, self.tr("Hint"), self.tr("A group with this name already exists"))
            return

        icon = self._get_selected_icon()
        if _qt_object_is_valid(getattr(self, "radio_hidden", None)) and self.radio_hidden.isChecked():
            group_type = GroupType.HIDDEN
        elif _qt_object_is_valid(getattr(self, "radio_file", None)) and self.radio_file.isChecked():
            group_type = GroupType.FILE
        else:
            group_type = GroupType.NORMAL

        result = save_group(self.manager, self.editing_group_id, name, icon, int(group_type))
        if not result.success:
            message = self.tr("Failed to create group") if result.error == "create_group_failed" else self.tr("Failed to update group")
            show_warning_dialog(self, self.tr("Failed"), message)
            return

        self.group_added.emit()
        self.data_changed.emit()
        self._refresh_group_list()
        if result.action == "created":
            self.group_name_input.clear()
            self.list_widget.setCurrentRow(0)

    def _save_content(self):
        """保存内容"""
        if self.selected_group_id is None:
            show_warning_dialog(self, self.tr("Hint"), self.tr("Please select a group first"))
            return

        if self._is_file_content_mode():
            self._save_file_content()
        else:
            self._save_text_content()

    def _save_file_content(self):
        """保存文件类型内容"""
        path = getattr(self, "selected_file_path", None)
        if not path and hasattr(self, "file_path_input"):
            path = self.file_path_input.text().strip()
        if not path:
            show_warning_dialog(self, self.tr("Hint"), self.tr("Please select a file"))
            return

        title = self.title_input.text().strip() if hasattr(self, "title_input") else None
        title = title if title else None

        result = save_file_content(self.manager, self.editing_item_id, self.selected_group_id, path, title)
        if not result.success:
            error_messages = {
                "move_to_group_failed": self.tr("Failed to move to group"),
                "add_content_failed": self.tr("Failed to add content"),
                "update_content_failed": self.tr("Failed to update content"),
            }
            show_warning_dialog(self, self.tr("Failed"), error_messages.get(result.error, self.tr("Failed to update content")))
            return

        if result.action == "created":
            if hasattr(self, "title_input"):
                self.title_input.clear()
            if hasattr(self, "file_path_input"):
                self.file_path_input.clear()
            self.selected_file_path = None
            self.list_widget.setCurrentRow(0)

        self.content_added.emit(self.selected_group_id)
        self.data_changed.emit()
        self._refresh_content_list()

    def _save_text_content(self):
        """保存文本类型内容（原有逻辑）"""
        content = self.content_edit.toPlainText().strip()
        if not content:
            show_warning_dialog(self, self.tr("Hint"), self.tr("Please enter content"))
            return

        title = self.title_input.text().strip() if hasattr(self, "title_input") else None
        title = title if title else None

        result = save_text_content(self.manager, self.editing_item_id, self.selected_group_id, content, title)
        if not result.success:
            error_messages = {
                "move_to_group_failed": self.tr("Failed to move to group"),
                "add_content_failed": self.tr("Failed to add content"),
                "update_content_failed": self.tr("Failed to update content"),
            }
            show_warning_dialog(self, self.tr("Failed"), error_messages.get(result.error, self.tr("Failed to update content")))
            return

        if result.action == "created":
            self.content_edit.clear()
            if hasattr(self, "title_input"):
                self.title_input.clear()
            self.list_widget.setCurrentRow(0)

        self.content_added.emit(self.selected_group_id)
        self.data_changed.emit()
        self._refresh_content_list()

    def _on_delete_clicked(self):
        """删除按钮点击"""
        if self.current_mode == "group" and self.editing_group_id:
            reply = show_confirm_dialog(self, self.tr("Confirm Delete"), self._get_delete_group_confirm_message())
            if reply:
                if self.manager.delete_group(self.editing_group_id):
                    self.editing_group_id = None
                    self.group_added.emit()
                    self.data_changed.emit()
                    self._refresh_group_list()
                    self._show_new_group_form()
                else:
                    show_warning_dialog(self, self.tr("Failed"), self.tr("Failed to delete group"))

        elif self.current_mode == "content" and self.editing_item_id:
            reply = show_confirm_dialog(self, self.tr("Confirm Delete"), self.tr("Are you sure you want to delete this item?"))
            if reply:
                if self.manager.delete_item(self.editing_item_id):
                    self.editing_item_id = None
                    self.content_added.emit(self.selected_group_id)
                    self.data_changed.emit()
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
        file_path, _ = QFileDialog.getSaveFileName(
            self,
            self.tr("Export to CSV"),
            "clipboard_export.csv",
            "CSV Files (*.csv);;All Files (*)",
        )

        if not file_path:
            return

        try:
            rows = collect_text_export_rows(self.manager)

            encoding = "utf-8-sig"
            if hasattr(self, "export_encoding_combo"):
                encoding = self.export_encoding_combo.currentData() or "utf-8-sig"

            write_csv_rows(file_path, [self.tr("Group"), self.tr("Content"), self.tr("Title")], rows, encoding)

            show_info_dialog(
                self,
                self.tr("Export Successful"),
                self.tr("Exported {count} items to CSV file.").format(count=len(rows)),
            )
        except Exception as e:
            show_warning_dialog(self, self.tr("Export Failed"), self.tr("Failed to export: {error}").format(error=str(e)))

    def _import_from_csv(self):
        """从 CSV 文件导入内容"""
        file_path, _ = QFileDialog.getOpenFileName(
            self,
            self.tr("Import from CSV"),
            "",
            "CSV Files (*.csv);;All Files (*)",
        )

        if not file_path:
            return

        try:
            rows = read_import_rows(file_path)

            if not rows:
                show_warning_dialog(self, self.tr("Import Failed"), self.tr("No valid data found in CSV file."))
                return

            imported_count = import_text_rows(self.manager, rows)

            self.group_added.emit()
            self.data_changed.emit()
            if self.current_mode == "group":
                self._refresh_group_list()
            else:
                self._refresh_content_list()

            show_info_dialog(
                self,
                self.tr("Import Successful"),
                self.tr("Imported {count} items.").format(count=imported_count),
            )
        except Exception as e:
            show_warning_dialog(self, self.tr("Import Failed"), self.tr("Failed to import: {error}").format(error=str(e)))


__all__ = [
    "ManageDialog",
    "get_existing_manage_dialog",
    "get_manage_dialog",
]
