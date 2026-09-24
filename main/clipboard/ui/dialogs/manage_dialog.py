# -*- coding: utf-8 -*-
"""
剪贴板管理窗口（独立窗口，单例）

左栏：分组列表；中栏：当前分组的内容列表；右栏：编辑区。
右栏编辑的对象由 current_mode 决定：
- "group"：新建 / 编辑分组
- "content"：新增 / 编辑内容
- "import_export"：CSV 导入导出
"""

import os
import time
from datetime import datetime
from typing import Optional

from PySide6.QtCore import Qt, QTimer, Signal
from PySide6.QtGui import QAction, QCursor, QIcon, QImage
from PySide6.QtWidgets import (
    QApplication,
    QFileDialog,
    QFrame,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QListWidgetItem,
    QMenu,
    QPushButton,
    QScrollArea,
    QStackedWidget,
    QVBoxLayout,
    QWidget,
)
from shiboken6 import isValid

from ui.fluent_lite import (
    FluentIcon,
    FluentTitleBar,
    FrostedFramelessDialog,
    LineEdit,
    SegmentedWidget,
    scrollbar_qss,
)
from ui.fluent_lite.theme import tinted_icon
from ui.settings_ui.components import (
    apply_theme_text_style,
    refresh_theme_widget_styles,
    theme_menu_style,
    theme_surface_color,
)
from core import safe_event
from core.ui_scale import configure_dialog_control, configure_dialog_controls, dialog_scaled
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
from ..forms.image_content_form import build_image_content_form, build_new_image_form
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
from ..image_item_actions import load_item_image, save_image_item_as
from ..layout_scale import (
    fit_manage_dialog_size,
    manage_dialog_height,
    manage_dialog_min_height,
    manage_dialog_min_width,
    manage_dialog_width,
)
from ..widgets.manage_rows import (
    ICON_ROLE,
    META_ROLE,
    SEARCH_ROLE,
    TAG_ROLE,
    THUMB_ROLE,
    ContentRowDelegate,
    GroupRowDelegate,
    clear_thumbnail_cache,
)
from ..widgets.reorder_list import ID_ROLE, ReorderListWidget


# 管理窗口一次载入分组的全部内容，拖拽排序才能拖到任意位置。
_ITEMS_PER_GROUP = 100000

# 新增图片要等剪贴板监听把它记进库：轮询间隔和最长等待时间
_IMAGE_CAPTURE_POLL_MS = 150
_IMAGE_CAPTURE_TIMEOUT_S = 3.0

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
    if not _qt_object_is_valid(_manage_window_instance):
        _manage_window_instance = None
    if _manage_window_instance is None:
        if manager is None:
            manager = ClipboardManager()
        _manage_window_instance = ManageDialog(manager)
    return _manage_window_instance


def get_existing_manage_dialog() -> Optional["ManageDialog"]:
    """获取已存在的管理窗口实例，不主动创建。"""
    global _manage_window_instance
    if not _qt_object_is_valid(_manage_window_instance):
        _manage_window_instance = None
    return _manage_window_instance


def destroy_manage_dialog() -> bool:
    """销毁缓存的管理窗口，返回它销毁前是否可见。"""
    global _manage_window_instance
    dialog = _manage_window_instance
    _manage_window_instance = None

    if not _qt_object_is_valid(dialog):
        return False

    was_visible = dialog.isVisible()
    # closeEvent() 有意只隐藏窗口并忽略关闭，因此这里必须绕过 close()，
    # 直接安排 QObject 销毁，才能让下次打开按新的构造期参数重建。
    dialog.hide()
    dialog.deleteLater()
    return was_visible


def _short_time(moment: Optional[datetime]) -> str:
    if moment is None:
        return ""
    now = datetime.now()
    if moment.date() == now.date():
        return moment.strftime("%H:%M")
    if moment.year == now.year:
        return moment.strftime("%m-%d")
    return moment.strftime("%Y-%m-%d")


class ManageDialog(FrostedFramelessDialog):
    """剪贴板管理窗口"""

    group_added = Signal()
    content_added = Signal(int)
    data_changed = Signal()

    def __init__(self, manager: ClipboardManager, parent=None):
        super().__init__(parent)
        self.manager = manager
        self.current_mode = "content"
        self.selected_group_id = None
        self.editing_group_id = None
        self.editing_item_id = None
        self._detail_form_token = 0
        # 通用分组里“新增内容”选的是文本、图片还是文件；快速启动分组固定是文件
        self._new_content_kind = "text"
        self._pending_image = None
        self._image_request = None
        # 拖动内容经过分组时判断能不能放，不在拖动中反复查库
        self._group_types = {}
        self._item_types = {}
        # 关窗口时放掉了列表和表单，下次显示要重新加载
        self._needs_reload = False
        self._image_wait_timer = QTimer(self)
        self._image_wait_timer.setInterval(_IMAGE_CAPTURE_POLL_MS)
        self._image_wait_timer.timeout.connect(self._poll_captured_image)

        self._setup_titlebar()

        self.setWindowTitle(self.tr("Clipboard Management"))
        self.setMinimumSize(manage_dialog_min_width(), manage_dialog_min_height())
        self.resize(manage_dialog_width(), manage_dialog_height())
        self.setWindowFlags(
            self.windowFlags()
            | Qt.WindowType.Window
            | Qt.WindowType.WindowCloseButtonHint
            | Qt.WindowType.WindowMinimizeButtonHint
            | Qt.WindowType.WindowMaximizeButtonHint
        )
        # setWindowFlags 会重建原生窗口，无边框库加的 WS_THICKFRAME 和阴影随之丢失，
        # 边缘就拖不动了，要重新加回去
        self.updateFrameless()
        icon = self._app_icon()
        if icon is not None:
            self.setWindowIcon(icon)
        self.setAttribute(Qt.WidgetAttribute.WA_DeleteOnClose, False)

        self._setup_ui()
        # Static Fluent controls are created once.  Opt them into the
        # standalone-window scale before any dynamic detail form is built.
        configure_dialog_controls(self)
        self._disable_auto_default(self)
        self._refresh_group_list()
        self._on_group_selected(self.selected_group_id)
        self._center_on_screen()
        self._apply_ui_theme(get_ui_theme().tokens)
        get_ui_theme().theme_changed.connect(self._apply_ui_theme)

    # ------------------------------------------------------------------
    # 窗口外壳
    # ------------------------------------------------------------------

    @staticmethod
    def _app_icon() -> Optional[QIcon]:
        try:
            from core.resource_manager import ResourceManager

            icon_path = ResourceManager.get_resource_path("svg/托盘.svg")
            if os.path.exists(icon_path):
                return QIcon(icon_path)
        except Exception:
            pass
        return None

    def _setup_titlebar(self):
        title_bar = FluentTitleBar(self)
        # Mark before _setup_ui(), which derives the content top margin from
        # titleBar.height(); an unscaled height puts the columns under the
        # caption buttons.
        configure_dialog_control(title_bar)
        self.setTitleBar(title_bar)
        title_bar.setDoubleClickEnabled(True)
        title_bar.center_title()

    def _center_on_screen(self):
        """在鼠标所在屏幕居中显示"""
        cursor_pos = QCursor.pos()
        screen = QApplication.screenAt(cursor_pos) or QApplication.primaryScreen()
        if screen:
            geo = screen.availableGeometry()
            self.resize(*fit_manage_dialog_size(geo.width(), geo.height()))
            self.move(
                geo.x() + (geo.width() - self.width()) // 2,
                geo.y() + (geo.height() - self.height()) // 2,
            )

    def show_and_activate(self):
        """显示并激活窗口（独立窗口专用方法）"""
        if self.isMinimized():
            self.showNormal()
        else:
            self.show()
        self.raise_()
        self.activateWindow()

    @safe_event
    def closeEvent(self, event):
        """关闭事件 - 只隐藏窗口，不销毁"""
        self.hide()
        event.ignore()

    @safe_event
    def hideEvent(self, event):
        super().hideEvent(event)
        # 点关闭、按 Esc（QDialog 走 reject 直接隐藏，不经过 closeEvent）都会到这里；
        # 最小化是系统发起的隐藏（spontaneous），窗口内容要保留
        if not event.spontaneous() and not self._needs_reload:
            self._release_memory()

    def _release_memory(self):
        """窗口隐藏后仍常驻，放掉大块数据：原图预览、待添加的图、整组内容和缩略图缓存。"""
        from core.logger import T, log_debug
        from core.platform_utils import request_trim_working_set

        self._clear_detail_layout()
        self._pending_image = None
        self.item_list.clear()
        self._item_types = {}
        clear_thumbnail_cache()
        self._needs_reload = True
        request_trim_working_set()
        log_debug(T("管理窗口已隐藏，已释放预览、列表和缩略图缓存"), "Clipboard")

    @safe_event
    def showEvent(self, event):
        super().showEvent(event)
        # 从外部定位打开（open_item_editor 等）时列表已提前加载，这里不会重复加载
        if self._needs_reload:
            self._refresh_group_list()
            self._on_group_selected(self.selected_group_id)

    # ------------------------------------------------------------------
    # 布局
    # ------------------------------------------------------------------

    def _setup_ui(self):
        s = dialog_scaled
        title_height = self.titleBar.height() if getattr(self, "titleBar", None) else s(32)
        main_layout = QHBoxLayout(self)
        main_layout.setContentsMargins(s(10), title_height + s(2), s(10), s(10))
        main_layout.setSpacing(s(10))

        self.sidebar = self._create_sidebar()
        main_layout.addWidget(self.sidebar)

        self.panel = QWidget()
        self.panel.setObjectName("ManagePanel")
        panel_layout = QHBoxLayout(self.panel)
        panel_layout.setContentsMargins(0, 0, 0, 0)
        panel_layout.setSpacing(0)

        self.list_column = self._create_list_column()
        panel_layout.addWidget(self.list_column)

        self.column_separator = QFrame()
        self.column_separator.setFixedWidth(1)
        panel_layout.addWidget(self.column_separator)

        self.detail_column = self._create_detail_column()
        panel_layout.addWidget(self.detail_column, 1)

        main_layout.addWidget(self.panel, 1)

    def _create_sidebar(self) -> QWidget:
        s = dialog_scaled
        sidebar = QWidget()
        sidebar.setObjectName("ManageSidebar")
        sidebar.setFixedWidth(s(228))
        layout = QVBoxLayout(sidebar)
        layout.setContentsMargins(s(4), s(4), s(4), s(4))
        layout.setSpacing(s(6))

        section = QHBoxLayout()
        section.setContentsMargins(s(12), s(6), s(6), 0)
        self.groups_caption = QLabel(self.tr("Groups"))
        apply_theme_text_style(self.groups_caption, 11, caption=True, extra="font-weight: 600;")
        section.addWidget(self.groups_caption, 1)
        self.new_group_btn = QPushButton("+ " + self.tr("New"))
        self.new_group_btn.setObjectName("ManageTextButton")
        self.new_group_btn.setToolTip(self.tr("New Group"))
        self.new_group_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        self.new_group_btn.clicked.connect(self._show_new_group_form)
        section.addWidget(self.new_group_btn)
        layout.addLayout(section)

        self.group_list = ReorderListWidget()
        self.group_list.setObjectName("ManageGroupList")
        self.group_list.setItemDelegate(GroupRowDelegate(self.group_list))
        self.group_list.currentItemChanged.connect(self._on_group_row_changed)
        self.group_list.itemDoubleClicked.connect(
            lambda item: self._show_edit_group_form(item.data(ID_ROLE))
        )
        self.group_list.item_moved.connect(self._on_group_moved)
        # 放下时内容列表还在自己的拖拽循环里，排队到拖拽结束后再移动、刷新它
        self.group_list.item_dropped_on.connect(
            self._move_item_to_group, Qt.ConnectionType.QueuedConnection
        )
        self.group_list.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        self.group_list.customContextMenuRequested.connect(self._show_group_menu)
        layout.addWidget(self.group_list, 1)

        self.import_export_btn = QPushButton(self.tr("Import / Export"))
        self.import_export_btn.setObjectName("ManageSidebarAction")
        self.import_export_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        self.import_export_btn.clicked.connect(lambda: self._switch_mode("import_export"))
        layout.addWidget(self.import_export_btn)
        return sidebar

    def _create_list_column(self) -> QWidget:
        s = dialog_scaled
        column = QWidget()
        column.setObjectName("ManageListColumn")
        column.setFixedWidth(s(340))
        layout = QVBoxLayout(column)
        layout.setContentsMargins(s(16), s(20), s(16), s(12))
        layout.setSpacing(s(10))

        header = QHBoxLayout()
        header.setContentsMargins(s(6), 0, 0, 0)
        header.setSpacing(s(6))
        title_box = QVBoxLayout()
        title_box.setSpacing(s(2))
        self.list_title = QLabel()
        apply_theme_text_style(self.list_title, 16, bold=True)
        self.list_count = QLabel()
        apply_theme_text_style(self.list_count, 11, caption=True)
        title_box.addWidget(self.list_title)
        title_box.addWidget(self.list_count)
        header.addLayout(title_box, 1)
        # QPushButton 的图标与文字间距不能用样式表调，用一个空格隔开
        self.edit_group_btn = QPushButton(" " + self.tr("Edit"))
        self.edit_group_btn.setObjectName("ManageTextButton")
        self.edit_group_btn.setToolTip(self.tr("Edit Group"))
        self.edit_group_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        self.edit_group_btn.clicked.connect(
            lambda: self.selected_group_id is not None
            and self._show_edit_group_form(self.selected_group_id)
        )
        header.addWidget(self.edit_group_btn, 0, Qt.AlignmentFlag.AlignTop)
        layout.addLayout(header)

        self.search_input = LineEdit()
        self.search_input.setPlaceholderText(self.tr("Search title or content"))
        self.search_input.setClearButtonEnabled(True)
        self._search_action = self.search_input.addAction(
            QIcon(), QLineEdit.ActionPosition.LeadingPosition
        )
        self.search_input.textChanged.connect(self._apply_search_filter)
        layout.addWidget(self.search_input)

        self.add_item_btn = QPushButton("+  " + self.tr("Add Content"))
        self.add_item_btn.setObjectName("ManageAddButton")
        self.add_item_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        self.add_item_btn.clicked.connect(self._on_add_item_clicked)
        layout.addWidget(self.add_item_btn)

        self.list_stack = QStackedWidget()
        self.item_list = ReorderListWidget()
        self.item_list.setObjectName("ManageItemList")
        self.item_list.setItemDelegate(ContentRowDelegate(self.item_list))
        self.item_list.itemClicked.connect(self._on_item_clicked)
        self.item_list.currentItemChanged.connect(self._on_item_row_changed)
        self.item_list.item_moved.connect(self._on_item_moved)
        self.item_list.drag_started.connect(lambda: self._on_content_drag(True))
        self.item_list.drag_finished.connect(lambda: self._on_content_drag(False))
        self.item_list.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        self.item_list.customContextMenuRequested.connect(self._show_item_menu)
        self.list_stack.addWidget(self.item_list)
        self.group_list.accept_items_from(self.item_list, self._can_move_item_to_group)

        self.empty_label = QLabel()
        self.empty_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.empty_label.setWordWrap(True)
        apply_theme_text_style(self.empty_label, 12, caption=True, extra=f"padding: {s(24)}px;")
        self.list_stack.addWidget(self.empty_label)
        layout.addWidget(self.list_stack, 1)

        self.reorder_hint = QLabel(self.tr("Drag or press Alt+Up/Down to reorder"))
        self.reorder_hint.setAlignment(Qt.AlignmentFlag.AlignCenter)
        apply_theme_text_style(self.reorder_hint, 10, caption=True)
        layout.addWidget(self.reorder_hint)
        return column

    def _create_detail_column(self) -> QWidget:
        s = dialog_scaled
        column = QWidget()
        column.setObjectName("ManageDetailColumn")
        layout = QVBoxLayout(column)
        layout.setContentsMargins(s(36), s(24), s(36), s(20))
        layout.setSpacing(s(6))

        self.detail_title = QLabel()
        apply_theme_text_style(self.detail_title, 18, bold=True)
        layout.addWidget(self.detail_title)

        self.detail_subtitle = QLabel()
        self.detail_subtitle.setWordWrap(True)
        apply_theme_text_style(self.detail_subtitle, 12, caption=True)
        self.detail_subtitle.hide()
        layout.addWidget(self.detail_subtitle)
        layout.addSpacing(s(12))

        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QFrame.Shape.NoFrame)
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        scroll.setStyleSheet("QScrollArea { background: transparent; border: none; }")
        self.detail_content = QWidget()
        self.detail_content.setObjectName("ManageDetailContent")
        self.detail_layout = QVBoxLayout(self.detail_content)
        self.detail_layout.setContentsMargins(0, 0, s(2), 0)
        self.detail_layout.setSpacing(s(8))
        scroll.setWidget(self.detail_content)
        layout.addWidget(scroll, 1)

        footer = QHBoxLayout()
        footer.setContentsMargins(0, s(12), 0, 0)
        footer.setSpacing(s(10))
        self.detail_meta = QLabel()
        apply_theme_text_style(self.detail_meta, 11, caption=True)
        footer.addWidget(self.detail_meta, 1)

        self.delete_btn = QPushButton(self.tr("Delete"))
        self.delete_btn.setObjectName("ManageDeleteButton")
        self.delete_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        self.delete_btn.clicked.connect(self._on_delete_clicked)
        self.delete_btn.hide()
        footer.addWidget(self.delete_btn)

        self.save_btn = QPushButton(self.tr("Save"))
        self.save_btn.setObjectName("ManageSaveButton")
        self.save_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        self.save_btn.clicked.connect(self._on_save_clicked)
        footer.addWidget(self.save_btn)
        layout.addLayout(footer)
        return column

    # ------------------------------------------------------------------
    # 主题
    # ------------------------------------------------------------------

    def _refresh_theme_scope(self, tokens=None):
        """Reapply Fluent styles after controls have acquired this parent."""
        tokens = tokens or get_ui_theme().tokens
        for widget in self.findChildren(QWidget):
            apply_theme = getattr(widget, "_apply_theme", None)
            if callable(apply_theme):
                apply_theme(tokens)
            else:
                # PySide6 6.9.x shadows QWidget.update() with the
                # QAbstractItemView.update(QModelIndex) overload on item views,
                # so a bare view.update() raises TypeError. Refresh the viewport
                # instead, which is a plain QWidget on every version.
                viewport = getattr(widget, "viewport", None)
                (viewport() if callable(viewport) else widget).update()

    def _apply_ui_theme(self, tokens):
        """Apply the same application theme used by the settings window."""
        s = dialog_scaled
        self._refresh_theme_scope(tokens)
        refresh_theme_widget_styles(self)
        self.setStyleSheet(f"""
            QWidget#ManagePanel {{
                background: {theme_surface_color()};
                border: 1px solid {tokens.separator};
                border-radius: {s(12)}px;
            }}
            QWidget#ManageSidebar, QWidget#ManageListColumn,
            QWidget#ManageDetailColumn, QWidget#ManageDetailContent {{
                background: transparent;
            }}
            QListWidget {{
                background: transparent;
                border: none;
                outline: none;
            }}
            QFrame#ManageSectionCard {{
                background: {tokens.input_background};
                border: 1px solid {tokens.separator};
                border-radius: {s(12)}px;
            }}
            QTextEdit {{
                border: 1px solid {tokens.border};
                border-radius: {s(8)}px;
                padding: {s(10)}px {s(12)}px;
                font-size: {s(13)}px;
                background: {tokens.input_background};
                color: {tokens.text};
                selection-background-color: {tokens.accent};
            }}
            QTextEdit:focus {{
                border-color: {tokens.accent};
            }}
            QPushButton#ManageTextButton {{
                background: transparent;
                border: none;
                border-radius: {s(8)}px;
                color: {tokens.accent_text};
                font-size: {s(12)}px;
                font-weight: 600;
                padding: {s(4)}px {s(10)}px;
            }}
            QPushButton#ManageTextButton:hover {{
                background: {tokens.accent_soft};
            }}
            QPushButton#ManageSidebarAction {{
                background: transparent;
                border: 1px solid transparent;
                border-radius: {s(8)}px;
                color: {tokens.text_muted};
                font-size: {s(12)}px;
                padding: {s(8)}px {s(12)}px;
                text-align: left;
            }}
            QPushButton#ManageSidebarAction:hover {{
                background: {tokens.accent_soft};
                color: {tokens.accent_text};
            }}
            QPushButton#ManageAddButton {{
                background: transparent;
                border: 1px dashed {tokens.border_hover};
                border-radius: {s(10)}px;
                color: {tokens.accent_text};
                font-size: {s(12)}px;
                font-weight: 600;
                min-height: {s(34)}px;
            }}
            QPushButton#ManageAddButton:hover {{
                background: {tokens.accent_soft};
                border-color: {tokens.accent};
            }}
            QPushButton#ManageDeleteButton {{
                background: transparent;
                border: 1px solid transparent;
                border-radius: {s(10)}px;
                color: {tokens.danger};
                font-size: {s(13)}px;
                min-height: {s(36)}px;
                padding: 0 {s(16)}px;
            }}
            QPushButton#ManageDeleteButton:hover {{
                background: {tokens.danger_soft};
            }}
            QPushButton#ManageSaveButton {{
                background: {tokens.accent_strong};
                border: 1px solid {tokens.accent_strong};
                border-radius: {s(10)}px;
                color: #FFFFFF;
                font-size: {s(13)}px;
                font-weight: 600;
                min-height: {s(36)}px;
                min-width: {s(104)}px;
                padding: 0 {s(22)}px;
            }}
            QPushButton#ManageSaveButton:hover {{
                background: {tokens.accent_strong_hover};
                border-color: {tokens.accent_strong_hover};
            }}
            QPushButton#ManageSaveButton:pressed {{
                background: {tokens.accent_strong_pressed};
            }}
        """ + scrollbar_qss(self))
        self.column_separator.setStyleSheet(f"background: {tokens.separator}; border: none;")
        self._search_action.setIcon(tinted_icon(FluentIcon.SEARCH, tokens.text_muted))
        self.import_export_btn.setIcon(tinted_icon(FluentIcon.SYNC, tokens.text_muted))
        self.edit_group_btn.setIcon(tinted_icon(FluentIcon.EDIT, tokens.accent_text))
        self._apply_dynamic_form_theme()
        self.update()

    def _apply_dynamic_form_theme(self):
        """Refresh controls that are rebuilt when the selected form changes."""
        refresh_theme_widget_styles(self.detail_content)
        for index, button in enumerate(getattr(self, "_emoji_tab_buttons", [])):
            button.setStyleSheet(
                emoji_tab_style(index == getattr(self, "_emoji_current_idx", 0), self)
            )

    # ------------------------------------------------------------------
    # 分组列表
    # ------------------------------------------------------------------

    def _group_tag(self, group: Group) -> str:
        if group.group_type == GroupType.FILE:
            return self.tr("Launch")
        if group.group_type == GroupType.HIDDEN:
            return self.tr("Hidden")
        return ""

    def _refresh_group_list(self):
        """重建分组列表，尽量保留当前选中的分组。"""
        groups = self.manager.get_groups()
        ids = [group.id for group in groups]
        self._group_types = {group.id: group.group_type for group in groups}
        if self.selected_group_id not in ids:
            self.selected_group_id = ids[0] if ids else None

        self.group_list.blockSignals(True)
        try:
            self.group_list.clear()
            for group in groups:
                item = QListWidgetItem(group.name)
                item.setData(ID_ROLE, group.id)
                item.setData(ICON_ROLE, get_group_display_icon(
                    group.icon,
                    group.group_type == GroupType.FILE,
                    is_hidden=(group.group_type == GroupType.HIDDEN),
                ))
                item.setData(TAG_ROLE, self._group_tag(group))
                self.group_list.addItem(item)
            row = self.group_list.row_of_id(self.selected_group_id)
            if row >= 0:
                self.group_list.setCurrentRow(row)
        finally:
            self.group_list.blockSignals(False)

    def _on_group_row_changed(self, current, _previous):
        if current is None:
            return
        self._on_group_selected(current.data(ID_ROLE))

    def _on_group_selected(self, group_id: Optional[int]):
        """切换到某个分组：刷新中栏，右栏回到该分组的新增内容表单。"""
        self.selected_group_id = group_id
        if self.search_input.text():
            self.search_input.blockSignals(True)
            self.search_input.clear()
            self.search_input.blockSignals(False)
        self._refresh_content_list()
        self._show_new_content_form()

    def _get_selected_group(self) -> Optional[Group]:
        """获取当前选中分组对象"""
        if self.selected_group_id is None:
            return None
        return next((g for g in self.manager.get_groups() if g.id == self.selected_group_id), None)

    def _on_group_moved(self, group_id: int, before_id, after_id):
        from core.logger import T, log_error

        if self.manager.move_group_between(group_id, before_id=before_id, after_id=after_id):
            self.group_added.emit()
            self.data_changed.emit()
            return
        log_error(T("拖拽分组移动失败"), "GroupDrag")
        self._refresh_group_list()

    def _show_group_menu(self, pos):
        item = self.group_list.itemAt(pos)
        if item is None:
            return
        self.group_list.setCurrentItem(item)
        group_id = item.data(ID_ROLE)
        menu = QMenu(self)
        menu.setStyleSheet(theme_menu_style())
        self._add_menu_action(menu, self.tr("Edit Group"), lambda: self._show_edit_group_form(group_id))
        menu.addSeparator()
        self._add_menu_action(menu, self.tr("Move to Top"), self.group_list.move_current_to_top)
        self._add_menu_action(menu, self.tr("Move to Bottom"), self.group_list.move_current_to_bottom)
        menu.addSeparator()
        self._add_menu_action(menu, self.tr("Delete Group"), lambda: self._delete_group(group_id))
        menu.exec(self.group_list.viewport().mapToGlobal(pos))

    @staticmethod
    def _add_menu_action(menu: QMenu, text: str, handler) -> QAction:
        action = menu.addAction(text)
        action.triggered.connect(lambda _checked=False: handler())
        return action

    # ------------------------------------------------------------------
    # 内容列表
    # ------------------------------------------------------------------

    def _item_row(self, item) -> QListWidgetItem:
        row = QListWidgetItem(item.display_text)
        row.setData(ID_ROLE, item.id)
        row.setData(ICON_ROLE, item.icon)
        row.setData(THUMB_ROLE, item.thumbnail or "")
        row.setData(META_ROLE, _short_time(item.created_at))
        if item.content_type == "file":
            row.setData(SEARCH_ROLE, extract_first_file_path_from_content(item.content))
        else:
            row.setData(SEARCH_ROLE, item.content or "")
        return row

    def _refresh_content_list(self):
        """重建中栏内容列表，尽量保留当前选中的内容。"""
        self._needs_reload = False
        group = self._get_selected_group()
        self.edit_group_btn.setVisible(group is not None)
        self.add_item_btn.setEnabled(group is not None)
        self.search_input.setEnabled(group is not None)

        self.item_list.blockSignals(True)
        try:
            self.item_list.clear()
            if group is None:
                self.list_title.setText(self.tr("No Group"))
                self.list_count.setText("")
                self._show_list_empty(self.tr("Create a group on the left first"))
                return

            icon = get_group_display_icon(
                group.icon,
                group.group_type == GroupType.FILE,
                is_hidden=(group.group_type == GroupType.HIDDEN),
            )
            self.list_title.setText(f"{icon}  {group.name}")
            items = self.manager.get_by_group(group.id, offset=0, limit=_ITEMS_PER_GROUP)
            self.list_count.setText(self.tr("{count} items").format(count=len(items)))
            self._item_types = {item.id: item.content_type for item in items}
            for item in items:
                self.item_list.addItem(self._item_row(item))
            row = self.item_list.row_of_id(self.editing_item_id)
            if row >= 0:
                self.item_list.setCurrentRow(row)
        finally:
            self.item_list.blockSignals(False)
        self._apply_search_filter(self.search_input.text())

    def _show_list_empty(self, text: str):
        self.empty_label.setText(text)
        self.list_stack.setCurrentWidget(self.empty_label)
        self.reorder_hint.hide()

    def _apply_search_filter(self, text: str):
        keyword = text.strip().casefold()
        if self.selected_group_id is None:
            return
        if self.item_list.count() == 0:
            self._show_list_empty(self.tr("No content in this group yet"))
            return

        visible = 0
        for row in range(self.item_list.count()):
            row_item = self.item_list.item(row)
            haystack = f"{row_item.text()} {row_item.data(SEARCH_ROLE) or ''}".casefold()
            hidden = bool(keyword) and keyword not in haystack
            self.item_list.setRowHidden(row, hidden)
            visible += not hidden

        total = self.item_list.count()
        self.list_count.setText(
            self.tr("{visible} of {count} items").format(visible=visible, count=total)
            if keyword else self.tr("{count} items").format(count=total)
        )
        # 过滤后列表里只剩部分行，相邻关系不完整，拖拽排序会落到错误位置。
        self.item_list.set_reorder_enabled(not keyword)
        self.reorder_hint.setVisible(not keyword)
        if visible:
            self.list_stack.setCurrentWidget(self.item_list)
        else:
            self._show_list_empty(self.tr("No matching content"))

    def _on_item_clicked(self, item: QListWidgetItem):
        self._open_item_if_needed(item.data(ID_ROLE))

    def _on_item_row_changed(self, current, _previous):
        # 键盘上下切换选中项时跟着打开编辑；鼠标点击由 itemClicked 处理。
        if current is not None and self.item_list.hasFocus():
            self._open_item_if_needed(current.data(ID_ROLE))

    def _open_item_if_needed(self, item_id: int):
        if item_id != self.editing_item_id or self.current_mode != "content":
            self._show_edit_content_form(item_id)

    def _on_item_moved(self, item_id: int, before_id, after_id):
        from core.logger import T, log_error

        if self.manager.move_item_between(item_id, before_id=before_id, after_id=after_id):
            self.content_added.emit(self.selected_group_id)
            self.data_changed.emit()
            return
        log_error(T("拖拽内容移动失败"), "ItemDrag")
        self._refresh_content_list()

    def _show_item_menu(self, pos):
        item = self.item_list.itemAt(pos)
        if item is None:
            return
        self.item_list.setCurrentItem(item)
        item_id = item.data(ID_ROLE)
        self._show_edit_content_form(item_id)
        menu = QMenu(self)
        menu.setStyleSheet(theme_menu_style())
        self._add_menu_action(menu, self.tr("Move to Top"), self.item_list.move_current_to_top)
        self._add_menu_action(menu, self.tr("Move to Bottom"), self.item_list.move_current_to_bottom)
        group_menu = menu.addMenu(self.tr("Move to Group"))
        group_menu.setStyleSheet(theme_menu_style())
        for group in self.manager.get_groups():
            if group.id == self.selected_group_id:
                continue
            icon = get_group_display_icon(
                group.icon, group.group_type == GroupType.FILE,
                is_hidden=(group.group_type == GroupType.HIDDEN),
            )
            action = self._add_menu_action(
                group_menu, f"{icon}  {group.name}",
                lambda gid=group.id: self._move_item_to_group(item_id, gid),
            )
            action.setEnabled(self._can_move_item_to_group(item_id, group.id))
        group_menu.setEnabled(not group_menu.isEmpty())
        menu.addSeparator()
        self._add_menu_action(menu, self.tr("Delete"), self._on_delete_clicked)
        menu.exec(self.item_list.viewport().mapToGlobal(pos))

    def _on_content_drag(self, dragging: bool):
        self.group_list.set_awaiting_foreign_drop(dragging, self.tr("Drop on a group to move it there"))

    def _can_move_item_to_group(self, item_id, group_id) -> bool:
        if group_id is None or group_id == self.selected_group_id:
            return False
        # 快速启动分组只收文件（ClipboardManager.move_to_group 也会拒绝）
        if self._group_types.get(group_id) == GroupType.FILE:
            return self._item_types.get(item_id) == "file"
        return True

    def _move_item_to_group(self, item_id, group_id):
        if not self._can_move_item_to_group(item_id, group_id):
            return
        if not self.manager.move_to_group(item_id, group_id):
            show_warning_dialog(self, self.tr("Failed"), self.tr("Failed to move to group"))
            return
        was_editing = self.editing_item_id == item_id
        if was_editing:
            self.editing_item_id = None
        self.content_added.emit(self.selected_group_id)
        self.content_added.emit(group_id)
        self.data_changed.emit()
        self._refresh_content_list()
        if was_editing:
            self._show_new_content_form()

    def _on_add_item_clicked(self):
        self.item_list.clearSelection()
        self.item_list.setCurrentRow(-1)
        self._show_new_content_form()

    # ------------------------------------------------------------------
    # 外部入口
    # ------------------------------------------------------------------

    def _switch_mode(self, mode: str):
        """右栏切到指定编辑对象：group 新建分组 / content 新增内容 / import_export。"""
        if mode == "group":
            self._show_new_group_form()
        elif mode == "import_export":
            self._show_import_export_form()
        else:
            self._show_new_content_form()

    def _switch_page(self, index: int):
        """剪贴板窗口的入口：0 新建分组，1 在当前分组里新增内容。"""
        self._switch_mode("group" if index == 0 else "content")

    def _select_group(self, group_id: Optional[int]):
        row = self.group_list.row_of_id(group_id)
        if row < 0:
            return
        if row == self.group_list.currentRow():
            self._on_group_selected(group_id)
        else:
            self.group_list.setCurrentRow(row)

    def refresh_after_external_change(self, deleted_group_id: Optional[int] = None):
        """外部数据变化后同步当前管理窗口，避免显示已删除的旧数据。"""
        self.setUpdatesEnabled(False)
        try:
            if deleted_group_id is not None:
                if self.editing_group_id == deleted_group_id:
                    self.editing_group_id = None
                if self.selected_group_id == deleted_group_id:
                    self.selected_group_id = None
            if self.editing_item_id is not None and self.manager.get_item(self.editing_item_id) is None:
                self.editing_item_id = None

            self._refresh_group_list()
            self._refresh_content_list()
            if self.current_mode == "group":
                if self.editing_group_id is not None:
                    self._show_edit_group_form(self.editing_group_id)
                else:
                    self._show_new_group_form()
            elif self.current_mode == "content":
                if self.editing_item_id is not None:
                    self._show_edit_content_form(self.editing_item_id)
                else:
                    self._show_new_content_form()
        finally:
            self.setUpdatesEnabled(True)

    def open_group_editor(self, group_id: int):
        """打开并定位到指定分组的编辑界面"""
        self.setUpdatesEnabled(False)
        try:
            self._refresh_group_list()
            self._select_group(group_id)
            self._show_edit_group_form(group_id)
        finally:
            self.setUpdatesEnabled(True)
        self.show_and_activate()

    def open_item_editor(self, item_id: int, group_id: Optional[int]):
        """打开并定位到指定内容的编辑界面"""
        self.setUpdatesEnabled(False)
        try:
            self._refresh_group_list()
            if group_id is not None:
                self._select_group(group_id)
            self.editing_item_id = item_id
            self._refresh_content_list()
            self._show_edit_content_form(item_id)
        finally:
            self.setUpdatesEnabled(True)
        self.show_and_activate()

    # ------------------------------------------------------------------
    # 右栏：表单切换
    # ------------------------------------------------------------------

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
            "image_preview",
            "content_kind_switch",
            "_editing_image_size",
            "image_drop_zone",
            "paste_image_btn",
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
        self.detail_meta.setText("")

        def clear_layout(layout):
            while layout.count():
                child = layout.takeAt(0)
                widget = child.widget()
                if widget:
                    # 刚加进布局的控件，Qt 会排队稍后显示；不先显式 hide，摘掉父控件后
                    # 那次排队的显示会把它当成独立窗口弹出来。
                    widget.hide()
                    widget.setParent(None)
                    widget.deleteLater()
                elif child.layout():
                    clear_layout(child.layout())

        clear_layout(self.detail_layout)

    def _finish_detail_form_setup(self):
        """Apply window scaling and theme to a freshly rebuilt detail form."""
        # Detail forms are destroyed and recreated when the selection or mode
        # changes, so the one-time setup in __init__ cannot cover these new
        # controls.  Mark only this subtree to avoid touching existing widgets
        # or compounding any geometry values.
        configure_dialog_controls(self.detail_content)
        self._disable_auto_default(self.detail_content)
        self._refresh_theme_scope()
        self._apply_dynamic_form_theme()

    @staticmethod
    def _disable_auto_default(root: QWidget):
        # QDialog 里的按钮默认 autoDefault，在搜索框、标题框里按回车会误触发删除、新建等按钮。
        for button in root.findChildren(QPushButton):
            button.setAutoDefault(False)

    def _is_current_detail_form_token(self, form_token: Optional[int]) -> bool:
        return form_token is None or form_token == self._detail_form_token

    def _set_detail_header(self, title: str, subtitle: str = ""):
        self.detail_title.setText(title)
        self.detail_subtitle.setText(subtitle)
        self.detail_subtitle.setVisible(bool(subtitle))

    def _set_detail_subtitle(self, text: str):
        self.detail_subtitle.setText(text)
        self.detail_subtitle.setVisible(bool(text))

    def _set_footer(self, save_text: Optional[str], deletable: bool):
        self.save_btn.setVisible(save_text is not None)
        if save_text is not None:
            self.save_btn.setText(save_text)
        self.delete_btn.setVisible(deletable)

    def _show_new_group_form(self):
        """显示新建分组表单"""
        self.current_mode = "group"
        self._clear_detail_layout()
        self._set_footer(self.tr("Create"), deletable=False)
        self.editing_group_id = None

        build_new_group_form(self)
        self._update_group_form_header()
        self._finish_detail_form_setup()
        self.group_name_input.setFocus()

    def _show_edit_group_form(self, group_id: int):
        """显示编辑分组表单"""
        group = next((g for g in self.manager.get_groups() if g.id == group_id), None)
        if not group:
            return
        self.current_mode = "group"
        self._clear_detail_layout()
        self._set_footer(self.tr("Save"), deletable=True)
        self.editing_group_id = group_id

        build_edit_group_form(self, group)
        self._update_group_form_header()
        self._finish_detail_form_setup()

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

    def _update_group_form_header(self):
        """根据当前分组类型刷新分组表单标题和说明。"""
        if not _qt_object_is_valid(getattr(self, "radio_file", None)):
            return

        is_new = self.editing_group_id is None
        if _qt_object_is_valid(getattr(self, "radio_hidden", None)) and self.radio_hidden.isChecked():
            title = self.tr("New Hidden Group") if is_new else self.tr("Edit Hidden Group")
            subtitle = self.tr("Hidden groups are not displayed on the clipboard panel; changing to normal group restores visibility")
        elif self.radio_file.isChecked():
            title = self.tr("New Quick Launch Group") if is_new else self.tr("Edit Quick Launch Group")
            subtitle = self.tr("Quick launch groups only allow file or folder paths; selecting an item opens them")
        else:
            title = self.tr("New General Group") if is_new else self.tr("Edit General Group")
            subtitle = self.tr("General groups allow any content; selecting an item pastes content or files to the target")
        self._set_detail_header(title, subtitle)

    def _create_emoji_picker(self, current_icon: str = "📁"):
        """创建 emoji 选择器（输入框 + 预览 + 分组标签页 + 可滚动网格）"""
        create_group_icon_picker(self, current_icon)

    def _emoji_tab_style(self, active: bool) -> str:
        return emoji_tab_style(active, self)

    def _emoji_btn_style(self) -> str:
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
        if selected_group.group_type == GroupType.FILE:
            return True
        return self._new_content_kind == "file"

    def _extract_file_path_from_item(self, item) -> str:
        """从文件条目内容中提取首个路径，兼容旧格式原始路径文本。"""
        if item is None or item.content_type != "file":
            return ""
        return extract_first_file_path_from_content(item.content)

    def _show_new_content_form(self):
        """显示新增内容表单"""
        self.current_mode = "content"
        self._clear_detail_layout()
        self.editing_item_id = None

        group = self._get_selected_group()
        if group is None:
            self._set_detail_header(self.tr("Add Content"))
            self._set_footer(None, deletable=False)
            hint = QLabel(self.tr("Please create a group first"))
            apply_theme_text_style(hint, 12, caption=True)
            self.detail_layout.addWidget(hint)
            self.detail_layout.addStretch()
            self._finish_detail_form_setup()
            return

        self._set_detail_header(
            self.tr("Add Content"),
            self.tr("Saved to \"{group}\"").format(group=group.name),
        )
        self._set_footer(self.tr("Add"), deletable=False)
        if group.group_type == GroupType.FILE:
            self._build_file_content_form()
        else:
            self._add_content_kind_switch()
            if self._new_content_kind == "file":
                self._build_file_content_form()
            elif self._new_content_kind == "image":
                self._build_new_image_form()
            else:
                self._build_text_content_form()
        self._finish_detail_form_setup()

    def _add_content_kind_switch(self):
        switch = SegmentedWidget()
        switch.addItem("text", self.tr("Text"))
        switch.addItem("image", self.tr("Image"))
        switch.addItem("file", self.tr("File"))
        switch.setCurrentItem(self._new_content_kind)
        switch.currentItemChanged.connect(self._set_new_content_kind)
        self.content_kind_switch = switch
        row = QHBoxLayout()
        row.setContentsMargins(0, 0, 0, dialog_scaled(6))
        row.addWidget(switch)
        row.addStretch()
        self.detail_layout.addLayout(row)

    def _set_new_content_kind(self, kind: str):
        if kind == self._new_content_kind:
            return
        # 切换类型时，已经填的标题带到新表单里
        title_input = getattr(self, "title_input", None)
        title = title_input.text() if _qt_object_is_valid(title_input) else ""
        self._new_content_kind = kind
        self._show_new_content_form()
        if title and _qt_object_is_valid(getattr(self, "title_input", None)):
            self.title_input.setText(title)

    def _build_text_content_form(self):
        """普通分组 — 文本内容输入表单"""
        build_text_content_form(self)

    def _build_new_image_form(self):
        monitoring = bool(getattr(self.manager, "is_monitoring", lambda: True)())
        self._pending_image = None
        build_new_image_form(self, monitoring)

    def _set_pending_image(self, image):
        self._pending_image = image if image is not None and not image.isNull() else None
        if _qt_object_is_valid(getattr(self, "image_drop_zone", None)):
            self.image_drop_zone.set_image(self._pending_image)

    def _use_clipboard_image(self):
        image = QApplication.clipboard().image()
        if image.isNull():
            show_warning_dialog(self, self.tr("Hint"), self.tr("There is no image on the clipboard"))
            return
        self._set_pending_image(image)

    def _browse_image_file(self):
        path, _ = QFileDialog.getOpenFileName(
            self, self.tr("Choose Image File"), "",
            self.tr("Images (*.png *.jpg *.jpeg *.bmp *.gif *.webp *.tif *.tiff *.ico)"),
        )
        if not path:
            return
        image = QImage(path)
        if image.isNull():
            show_warning_dialog(self, self.tr("Hint"), self.tr("This file could not be opened as an image"))
            return
        self._set_pending_image(image)

    def _build_file_content_form(self, add_stretch: bool = True):
        """文件分组 — 文件选择表单"""
        build_file_content_form(self, add_stretch=add_stretch)

    def _set_selected_file_path(self, path: str):
        """统一设置当前选中的文件或文件夹路径。"""
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
        item = self.manager.get_item(item_id)
        if not item:
            return
        self.current_mode = "content"
        self._clear_detail_layout()
        self._set_detail_header(self.tr("Edit Content"))
        self._set_footer(self.tr("Save"), deletable=True)
        self.editing_item_id = item_id

        row = self.item_list.row_of_id(item_id)
        if row >= 0 and self.item_list.currentRow() != row:
            self.item_list.blockSignals(True)
            self.item_list.setCurrentRow(row)
            self.item_list.blockSignals(False)

        if item.content_type == "file":
            build_edit_file_content_form(self, item, self._extract_file_path_from_item(item))
            self.detail_layout.addStretch()
        elif item.content_type == "image":
            data = self.manager.get_image_data(item.image_id) if item.image_id else None
            image = QImage.fromData(data) if data else None
            if image is not None and image.isNull():
                image = None
            self._editing_image_size = (image.width(), image.height()) if image is not None else None
            build_image_content_form(self, item, image, len(data or b""))
        else:
            build_edit_text_content_form(self, item)

        if item.created_at:
            self.detail_meta.setText(
                self.tr("Created {time}").format(time=item.created_at.strftime("%Y-%m-%d %H:%M"))
            )
        self._finish_detail_form_setup()

    def _show_import_export_form(self):
        """显示导入导出表单"""
        self.current_mode = "import_export"
        self._clear_detail_layout()
        self._set_detail_header(
            self.tr("Import/Export"),
            self.tr("Back up saved text content as CSV, or import it on another computer"),
        )
        self._set_footer(None, deletable=False)
        self.item_list.clearSelection()

        build_import_export_form(self)
        self._finish_detail_form_setup()

    # ------------------------------------------------------------------
    # 保存 / 删除
    # ------------------------------------------------------------------

    def _select_icon(self, btn: QPushButton):
        """选择图标（旧方法，保留兼容）"""
        for b in self.icon_buttons:
            b.setChecked(b == btn)

    def _get_selected_icon(self) -> str:
        """获取选中的图标（优先从输入框获取，确保只有一个字符）"""
        if _qt_object_is_valid(getattr(self, "icon_input", None)):
            text = self.icon_input.text().strip()
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
        elif self.current_mode == "content":
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
        if result.action == "created" and result.entity_id is not None:
            self.selected_group_id = result.entity_id
        self._refresh_group_list()
        if result.action == "created":
            self._on_group_selected(self.selected_group_id)
        else:
            self._refresh_content_list()
            self._show_edit_group_form(self.editing_group_id)

    def _save_content(self):
        """保存内容"""
        if self.selected_group_id is None:
            show_warning_dialog(self, self.tr("Hint"), self.tr("Please select a group first"))
            return

        item = self.manager.get_item(self.editing_item_id) if self.editing_item_id is not None else None
        group = self._get_selected_group()
        if item is not None and item.content_type == "image":
            self._save_image_title(item)
        elif (item is None and self._new_content_kind == "image"
              and group is not None and group.group_type != GroupType.FILE):
            self._save_new_image()
        elif self._is_file_content_mode():
            self._save_file_content()
        else:
            self._save_text_content()

    def _save_image_title(self, item):
        """图片内容不可编辑，只保存标题。

        内容按原图尺寸重写成 [宽x高]：旧版把图片条目当文本编辑，内容可能已被改坏，保存一次即可恢复。
        """
        title = self.title_input.text().strip() or None
        size = getattr(self, "_editing_image_size", None)
        content = f"[{size[0]}x{size[1]}]" if size else item.content
        if not self.manager.update_item(item.id, content, title=title):
            show_warning_dialog(self, self.tr("Failed"), self.tr("Failed to update content"))
            return
        self.content_added.emit(self.selected_group_id)
        self.data_changed.emit()
        self._refresh_content_list()

    def _save_new_image(self):
        """新增图片：写进剪贴板，等监听把它记进库，再把那条记录移进当前分组。"""
        from core.clipboard_utils import copy_image_to_clipboard

        image = self._pending_image
        if image is None:
            show_warning_dialog(self, self.tr("Hint"), self.tr("Please choose an image first"))
            return
        if self._image_request is not None:
            return
        self._image_request = {
            "group_id": self.selected_group_id,
            "title": self.title_input.text().strip() or None,
            "content": f"[{image.width()}x{image.height()}]",
            "since": int(time.time()),
            "deadline": time.monotonic() + _IMAGE_CAPTURE_TIMEOUT_S,
        }
        # 禁用带焦点的按钮时 Qt 会把焦点挪给下一个控件，落到内容列表会自动选中首行、切走当前表单
        self.title_input.setFocus()
        self.save_btn.setEnabled(False)
        copy_image_to_clipboard(image)
        self._image_wait_timer.start()

    def _poll_captured_image(self):
        request = self._image_request
        if request is None:
            self._image_wait_timer.stop()
            return
        item = self._find_captured_image(request)
        if item is None and time.monotonic() < request["deadline"]:
            return
        self._image_wait_timer.stop()
        self._image_request = None
        self.save_btn.setEnabled(True)
        if item is None:
            show_warning_dialog(
                self, self.tr("Failed"),
                self.tr("The image was not recorded. Make sure clipboard history is turned on."),
            )
            return
        self._file_captured_image(item, request)

    def _find_captured_image(self, request):
        # 刚记下或被去重顶上来的那条 updated_at 不早于写入时刻；尺寸再对一次，免得认错别的图
        for item in self.manager.get_history(0, 30, content_type="image"):
            if (item.content == request["content"] and item.updated_at is not None
                    and item.updated_at.timestamp() >= request["since"]):
                return item
        return None

    def _group_of_item(self, item_id: int) -> Optional[Group]:
        for group in self.manager.get_groups():
            if any(item.id == item_id for item in self.manager.get_by_group(group.id, 0, _ITEMS_PER_GROUP)):
                return group
        return None

    def _file_captured_image(self, item, request):
        target_id = request["group_id"]
        target = next((g for g in self.manager.get_groups() if g.id == target_id), None)
        if target is None:
            return
        current = self._group_of_item(item.id)
        # 同一张图库里只有一条（按内容去重），已在别的分组时只能移过来，不能复制一份
        if current is not None and current.id != target_id and not show_confirm_dialog(
            self, self.tr("Image Already Saved"),
            self.tr("This image is already in \"{group}\". Move it to \"{target}\"?").format(
                group=current.name, target=target.name),
        ):
            return
        if request["title"]:
            self.manager.update_item(item.id, item.content, title=request["title"])
        if (current is None or current.id != target_id) and not self.manager.move_to_group(item.id, target_id):
            show_warning_dialog(self, self.tr("Failed"), self.tr("Failed to move to group"))
            return

        self.content_added.emit(target_id)
        self.data_changed.emit()
        if self.selected_group_id != target_id:
            return
        self._refresh_content_list()
        row = self.item_list.row_of_id(item.id)
        if row >= 0:
            self.item_list.scrollToItem(self.item_list.item(row))
        still_adding_image = (
            self.current_mode == "content" and self.editing_item_id is None
            and self._new_content_kind == "image"
        )
        if still_adding_image:
            self._set_pending_image(None)
            if _qt_object_is_valid(getattr(self, "title_input", None)):
                self.title_input.clear()

    def _editing_image_item(self):
        item = self.manager.get_item(self.editing_item_id) if self.editing_item_id is not None else None
        return item if item is not None and item.content_type == "image" else None

    def _copy_image_item(self):
        from core.clipboard_utils import copy_image_to_clipboard

        image = load_item_image(self.manager, self._editing_image_item())
        if image is not None:
            copy_image_to_clipboard(image)

    def _pin_image_item(self):
        from ..windows.pin_window import create_pin_from_clipboard_item

        item = self._editing_image_item()
        if item is not None:
            create_pin_from_clipboard_item(item.id, self.manager, self)

    def _save_image_item_as(self):
        save_image_item_as(self, self.manager, self._editing_image_item())

    def _content_save_failed(self, result):
        error_messages = {
            "move_to_group_failed": self.tr("Failed to move to group"),
            "add_content_failed": self.tr("Failed to add content"),
            "update_content_failed": self.tr("Failed to update content"),
        }
        show_warning_dialog(self, self.tr("Failed"), error_messages.get(result.error, self.tr("Failed to update content")))

    def _after_content_saved(self, result):
        self.content_added.emit(self.selected_group_id)
        self.data_changed.emit()
        self._refresh_content_list()
        if result.action == "created":
            # 新增后留在新增表单，方便连续录入；新条目在列表里高亮一下。
            row = self.item_list.row_of_id(result.entity_id)
            if row >= 0:
                self.item_list.scrollToItem(self.item_list.item(row))

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
            self._content_save_failed(result)
            return

        if result.action == "created":
            if hasattr(self, "title_input"):
                self.title_input.clear()
            if hasattr(self, "file_path_input"):
                self.file_path_input.clear()
            self.selected_file_path = None
        self._after_content_saved(result)

    def _save_text_content(self):
        """保存文本类型内容"""
        content = self.content_edit.toPlainText().strip()
        if not content:
            show_warning_dialog(self, self.tr("Hint"), self.tr("Please enter content"))
            return

        title = self.title_input.text().strip() if hasattr(self, "title_input") else None
        title = title if title else None

        result = save_text_content(self.manager, self.editing_item_id, self.selected_group_id, content, title)
        if not result.success:
            self._content_save_failed(result)
            return

        if result.action == "created":
            self.content_edit.clear()
            if hasattr(self, "title_input"):
                self.title_input.clear()
        self._after_content_saved(result)

    def _delete_group(self, group_id: int):
        if not show_confirm_dialog(self, self.tr("Confirm Delete"), self._get_delete_group_confirm_message()):
            return
        if not self.manager.delete_group(group_id):
            show_warning_dialog(self, self.tr("Failed"), self.tr("Failed to delete group"))
            return
        if self.editing_group_id == group_id:
            self.editing_group_id = None
        if self.selected_group_id == group_id:
            self.selected_group_id = None
        self.group_added.emit()
        self.data_changed.emit()
        self._refresh_group_list()
        self._on_group_selected(self.selected_group_id)
        self._show_new_group_form()

    def _on_delete_clicked(self):
        """删除按钮点击"""
        if self.current_mode == "group" and self.editing_group_id:
            self._delete_group(self.editing_group_id)

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

    # ------------------------------------------------------------------
    # 导入 / 导出
    # ------------------------------------------------------------------

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
            self._refresh_group_list()
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
    "destroy_manage_dialog",
    "get_existing_manage_dialog",
    "get_manage_dialog",
]
