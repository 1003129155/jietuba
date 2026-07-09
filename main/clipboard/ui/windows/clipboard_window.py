# -*- coding: utf-8 -*-
"""
剪贴板历史窗口

提供的剪贴板历史管理界面。
"""

import ctypes
import os
import re
from time import perf_counter
from typing import List, Optional

from PySide6.QtCore import QDate, QEvent, QLocale, QPoint, QSettings, QSize, QTimer, Qt, Signal
from PySide6.QtGui import QColor, QCursor, QPixmap
from PySide6.QtWidgets import (
    QApplication,
    QCalendarWidget,
    QDateEdit,
    QFrame,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QMenu,
    QPushButton,
    QSizePolicy,
    QToolButton,
    QVBoxLayout,
    QWidget,
)

from core import safe_event
from core.logger import log_debug, log_error, log_exception
from core.shortcut_manager import ShortcutHandler, ShortcutManager

from ...controllers import ClipboardController, SelectionManager, get_foreground_window, send_ctrl_v, set_foreground_window
from ...core import ClipboardItem, ClipboardManager, GroupType
from ..theme.theme_styles import ThemeStyleGenerator
from ..theme.themes import Theme, get_theme_manager
from ..mixins.frameless_mixin import FramelessMixin
from ..dialogs.manage_dialog import ManageDialog, get_manage_dialog
from ..menus.item_context_menu import ClipboardItemContextMenu
from ..widgets.group_bar import GroupBar
from ..widgets.item_delegate import ClipboardItemDelegate, ROLE_ITEM_DATA, ROLE_ITEM_ID
from ..widgets.preview_popup import PreviewPopup


class ClipboardShortcutHandler(ShortcutHandler):
    """剪贴板窗口快捷键处理器 (priority=60)"""

    def __init__(self, window: "ClipboardWindow"):
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
            if hasattr(w, "selection_manager") and w.selection_manager._selected_index >= 0:
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

        if key == Qt.Key.Key_F and modifiers == Qt.KeyboardModifier.ControlModifier:
            w.search_input.setFocus()
            return True

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
    """剪贴板历史窗口。"""

    item_pasted = Signal(int)
    closed = Signal()
    new_item_received = Signal()
    _offscreen_warmup_done = False

    def __init__(self, parent=None):
        super().__init__(parent)
        self.manager = ClipboardManager()
        self.controller = ClipboardController(self.manager)
        self.controller.data_loaded.connect(self._on_data_loaded)
        self.controller.loading_state_changed.connect(self._on_loading_changed)
        self.controller.reload_required.connect(self._on_reload_required)
        self.controller.load_completed.connect(self._on_load_completed)

        self.selected_item_id: Optional[int] = None
        self._is_loading = False
        self._ignore_manage_refresh_when_hidden = True
        self._auto_fill_max_pages = 3
        self._auto_fill_remaining = self._auto_fill_max_pages
        self._offscreen_warmup_in_progress = False

        self.new_item_received.connect(self._on_new_item)
        self._init_frameless(edge_margin=8)
        self._qsettings = QSettings("Jietuba", "ClipboardWindow")

        self.theme_manager = get_theme_manager()
        self.current_theme = self.theme_manager.get_current_theme()
        self.theme_manager.theme_changed.connect(self._on_theme_changed)
        self.theme_manager.font_size_changed.connect(self._on_font_size_changed)
        self.theme_manager.opacity_changed.connect(self._on_opacity_changed)
        try:
            from core.i18n import I18nManager

            I18nManager.instance().language_changed.connect(self._on_language_changed)
        except Exception as e:
            log_debug(f"连接剪贴板语言切换信号失败: {e}", "I18n")

        self._load_settings()
        self._load_window_geometry()
        self._setup_ui()
        self._apply_opacity()
        self._setup_shortcuts()

        PreviewPopup.instance().set_manager(self.manager)
        self._warmup_offscreen_once()

    def _on_data_loaded(self, items: List[ClipboardItem], is_first_page: bool):
        if is_first_page:
            self._auto_fill_remaining = self._auto_fill_max_pages
            if items or self.controller.current_items:
                self._refresh_list()
            else:
                self.list_widget.clear()
        else:
            self._append_items(items)

    def _on_loading_changed(self, is_loading: bool):
        self._is_loading = is_loading

    def _on_reload_required(self):
        self.controller.load_history()
        self.group_bar.refresh_buttons()

    def request_data_refresh(self, reason: str = ""):
        if self._ignore_manage_refresh_when_hidden and not self.isVisible():
            reason_text = reason or "Clipboard data"
            log_debug(f"{reason_text} 当前窗口不可见，跳过刷新界面", "Clipboard")
            return
        self._load_history()

    def _on_manage_data_changed(self):
        self.request_data_refresh("Manage dialog data")

    def _on_theme_changed(self, theme: Theme):
        self.current_theme = theme
        self._apply_theme()

    def _on_language_changed(self, _lang_code: str):
        self._apply_date_filter_locale()
        self._retranslate_ui()

    def _apply_theme(self):
        if hasattr(self, "_item_delegate"):
            self._item_delegate.set_theme(self.current_theme)
            self._item_delegate.set_window_opacity(self.window_opacity)
        self._apply_opacity()
        self.group_bar.set_theme(self.current_theme)
        if hasattr(self, "search_input"):
            has_text = bool(self.search_input.text().strip())
            self._apply_search_input_style(has_text)
            self._apply_clear_search_btn_style()
            self._apply_menu_btn_style()
            self._apply_time_filter_styles()
        self._refresh_list()
        log_debug(f"主题已切换到: {self.current_theme.display_name}", "Clipboard")

    def _load_settings(self):
        from settings import get_tool_settings_manager

        self.config = get_tool_settings_manager()
        self.window_opacity = self.config.get_clipboard_window_opacity()
        self.display_lines = self.config.get_clipboard_font_size()
        self.group_bar_position = self.config.get_clipboard_group_bar_position()

    def _current_date_locale(self) -> QLocale:
        try:
            from core.i18n import I18nManager

            lang_code = I18nManager.get_current_language()
        except Exception:
            lang_code = "zh"

        locale_map = {
            "zh": QLocale(QLocale.Language.Chinese, QLocale.Country.China),
            "ja": QLocale(QLocale.Language.Japanese, QLocale.Country.Japan),
            "ko": QLocale(QLocale.Language.Korean, QLocale.Country.SouthKorea),
            "en": QLocale(QLocale.Language.English, QLocale.Country.UnitedStates),
        }
        return locale_map.get(lang_code, locale_map["zh"])

    def _apply_date_filter_locale(self):
        if not hasattr(self, "start_date_edit"):
            return

        locale = self._current_date_locale()
        for date_edit in (self.start_date_edit, self.end_date_edit):
            date_edit.setLocale(locale)
            calendar = date_edit.calendarWidget()
            if calendar is not None:
                calendar.setLocale(locale)

    def _retranslate_ui(self):
        if not hasattr(self, "search_input"):
            return

        self.setWindowTitle(self.tr("Clipboard History"))
        self.start_date_edit.setToolTip(self.tr("Start date"))
        self.end_date_edit.setToolTip(self.tr("End date"))
        self.apply_time_filter_btn.setToolTip(self.tr("Apply time filter"))
        self.type_filter_btn.setToolTip(self.tr("Filter Type"))
        self.clear_time_filter_btn.setToolTip(self.tr("Close filters"))
        self.search_input.setPlaceholderText("🔍 " + self.tr("Search"))
        self.clear_search_btn.setToolTip(self.tr("Clear search"))

        self.type_filter_labels = [self.tr("All"), self.tr("Text"), self.tr("Image"), self.tr("File")]
        for index, action in enumerate(self.type_filter_actions):
            action.setText(self.type_filter_labels[index])
        self._update_type_filter_button(self.type_filter_index)
        if hasattr(self, "group_bar"):
            self.group_bar.refresh_buttons()

    def _apply_opacity(self):
        generator = ThemeStyleGenerator(self.current_theme)

        if hasattr(self, "container"):
            self.container.setStyleSheet(generator.generate_window_style(self.window_opacity))

        if hasattr(self, "list_widget"):
            self.list_widget.setStyleSheet(generator.generate_list_widget_style(self.window_opacity))

        if hasattr(self, "bottom_bar"):
            self.bottom_bar.setStyleSheet(generator.generate_search_bar_style(self.window_opacity))

        if hasattr(self, "time_filter_bar"):
            self.time_filter_bar.setStyleSheet(generator.generate_time_filter_bar_style(self.window_opacity))

        if hasattr(self, "group_bar") and self.group_bar.bar_widget is not None:
            border_dir = {"left": "border-right:", "top": "border-bottom:", "right": "border-left:"}
            bd = border_dir.get(getattr(self, "group_bar_position", "right"), "border-left:")
            self.group_bar.bar_widget.setStyleSheet(
                generator.generate_search_bar_style(self.window_opacity).replace("border-top:", bd)
            )

    def _load_window_geometry(self):
        try:
            from settings import get_tool_settings_manager

            config = get_tool_settings_manager()
            default_width = config.get_app_setting("clipboard_window_width", 450)
            default_height = config.get_app_setting("clipboard_window_height", 600)
        except Exception as e:
            log_exception(e, "加载剪贴板窗口几何设置")
            default_width = 450
            default_height = 600

        self._saved_x = self._qsettings.value("window/x", None)
        self._saved_y = self._qsettings.value("window/y", None)
        self._saved_width = self._qsettings.value("window/width", default_width, type=int)
        self._saved_height = self._qsettings.value("window/height", default_height, type=int)

        if self._saved_x is not None:
            self._saved_x = int(self._saved_x)
        if self._saved_y is not None:
            self._saved_y = int(self._saved_y)

    def _save_window_geometry(self):
        self._qsettings.setValue("window/x", self.x())
        self._qsettings.setValue("window/y", self.y())
        self._qsettings.setValue("window/width", self.width())
        self._qsettings.setValue("window/height", self.height())
        self._qsettings.sync()

    def _setup_ui(self):
        self.setWindowTitle(self.tr("Clipboard History"))
        self.setWindowFlags(
            Qt.WindowType.Window
            | Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.WindowStaysOnTopHint
            | Qt.WindowType.Tool
        )
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.setMinimumSize(390, 400)
        self.resize(self._saved_width, self._saved_height)

        self.container = QFrame(self)
        self.container.setStyleSheet(
            """
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
        """
        )
        self.container.setObjectName("mainContainer")

        main_layout = QVBoxLayout(self)
        main_layout.setContentsMargins(0, 0, 0, 0)
        main_layout.addWidget(self.container)

        self.content_layout = QHBoxLayout(self.container)
        self.content_layout.setContentsMargins(0, 0, 0, 0)
        self.content_layout.setSpacing(0)

        self.left_widget = QWidget()
        self.left_widget.setStyleSheet("background: transparent;")
        self.left_layout = QVBoxLayout(self.left_widget)
        self.left_layout.setContentsMargins(0, 0, 0, 0)
        self.left_layout.setSpacing(0)

        self.list_widget = QListWidget()
        self.list_widget.setStyleSheet(
            """
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
                background: transparent;
            }
            QListWidget::item:hover {
                background: transparent;
            }
        """
        )
        self.list_widget.setSpacing(0)
        self.list_widget.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.list_widget.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAsNeeded)
        self.list_widget.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        self.list_widget.customContextMenuRequested.connect(self._show_context_menu)

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
        self.list_widget.setMouseTracking(True)
        self.list_widget.viewport().setMouseTracking(True)
        self.list_widget.itemEntered.connect(self._on_delegate_item_entered)
        self.list_widget.viewport().installEventFilter(self)

        self.selection_manager = SelectionManager(self.list_widget, self._get_item_data)
        self.selection_manager.group_bar_position = self.group_bar_position
        self.selection_manager.item_activated.connect(self._on_paste_item)
        self.selection_manager.request_sidebar_focus.connect(self._enter_sidebar_mode)
        self.selection_manager.request_group_switch.connect(self._on_top_group_switch)

        self.list_widget.itemSelectionChanged.connect(self._on_selection_changed)

        scrollbar = self.list_widget.verticalScrollBar()
        scrollbar.valueChanged.connect(self._on_scroll)
        scrollbar.rangeChanged.connect(self._on_scrollbar_range_changed)

        self.left_layout.addWidget(self.list_widget, 1)

        self.time_filter_bar = QWidget()
        self.time_filter_bar.setFixedHeight(36)
        self.time_filter_bar.hide()
        time_filter_layout = QHBoxLayout(self.time_filter_bar)
        time_filter_layout.setContentsMargins(8, 4, 8, 4)
        time_filter_layout.setSpacing(5)

        today = QDate.currentDate()
        self.start_date_edit = QDateEdit(today.addDays(-7))
        self.start_date_edit.setCalendarPopup(True)
        self.start_date_edit.setDisplayFormat("yyyy/MM/dd")
        self.start_date_edit.setFixedSize(114, 26)
        self.start_date_edit.setToolTip(self.tr("Start date"))
        self.start_date_edit.setFocusPolicy(Qt.FocusPolicy.ClickFocus)
        time_filter_layout.addWidget(self.start_date_edit)

        self.time_filter_separator = QLabel("-")
        self.time_filter_separator.setAlignment(Qt.AlignmentFlag.AlignCenter)
        time_filter_layout.addWidget(self.time_filter_separator)

        self.end_date_edit = QDateEdit(today)
        self.end_date_edit.setCalendarPopup(True)
        self.end_date_edit.setDisplayFormat("yyyy/MM/dd")
        self.end_date_edit.setFixedSize(114, 26)
        self.end_date_edit.setToolTip(self.tr("End date"))
        self.end_date_edit.setFocusPolicy(Qt.FocusPolicy.ClickFocus)
        time_filter_layout.addWidget(self.end_date_edit)
        self._apply_date_filter_locale()

        self.apply_time_filter_btn = QToolButton()
        self.apply_time_filter_btn.setText("OK")
        self.apply_time_filter_btn.setFixedSize(34, 26)
        self.apply_time_filter_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        self.apply_time_filter_btn.setToolTip(self.tr("Apply time filter"))
        self.apply_time_filter_btn.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.apply_time_filter_btn.clicked.connect(self._apply_time_range_filter)
        time_filter_layout.addWidget(self.apply_time_filter_btn)

        time_filter_layout.addStretch(1)

        self.type_filter_labels = [self.tr("All"), self.tr("Text"), self.tr("Image"), self.tr("File")]
        self.type_filter_index = 0
        self.type_filter_btn = QToolButton()
        self.type_filter_btn.setFixedSize(66, 26)
        self.type_filter_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        self.type_filter_btn.setToolTip(self.tr("Filter Type"))
        self.type_filter_btn.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.type_filter_btn.clicked.connect(self._show_type_filter_menu)
        self.type_filter_menu = QMenu(self.type_filter_btn)
        self.type_filter_actions = []
        for idx, label in enumerate(self.type_filter_labels):
            action = self.type_filter_menu.addAction(label)
            action.setCheckable(True)
            action.setChecked(idx == self.type_filter_index)
            action.triggered.connect(lambda _checked=False, i=idx: self._set_type_filter(i))
            self.type_filter_actions.append(action)
        self._update_type_filter_button(0)
        time_filter_layout.addWidget(self.type_filter_btn)

        self.clear_time_filter_btn = QToolButton()
        self.clear_time_filter_btn.setText("X")
        self.clear_time_filter_btn.setFixedSize(26, 26)
        self.clear_time_filter_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        self.clear_time_filter_btn.setToolTip(self.tr("Close filters"))
        self.clear_time_filter_btn.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.clear_time_filter_btn.clicked.connect(self._close_time_filter_bar)
        time_filter_layout.addWidget(self.clear_time_filter_btn)
        self.left_layout.addWidget(self.time_filter_bar)

        self.bottom_bar = QWidget()
        self.bottom_bar.setFixedHeight(36)
        self.bottom_bar.setStyleSheet(
            """
            QWidget {
                background: #FAFAFA;
                border-top: 1px solid #E0E0E0;
            }
        """
        )
        bottom_layout = QHBoxLayout(self.bottom_bar)
        bottom_layout.setContentsMargins(8, 4, 8, 4)
        bottom_layout.setSpacing(8)

        self.time_filter_toggle_btn = QToolButton()
        self.time_filter_toggle_btn.setArrowType(Qt.ArrowType.UpArrow)
        self.time_filter_toggle_btn.setFixedSize(28, 28)
        self.time_filter_toggle_btn.setCheckable(True)
        self.time_filter_toggle_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        self.time_filter_toggle_btn.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.time_filter_toggle_btn.clicked.connect(self._toggle_time_filter_bar)
        bottom_layout.addWidget(self.time_filter_toggle_btn)

        self.search_input = QLineEdit()
        self.search_input.setPlaceholderText("🔍 " + self.tr("Search"))
        self._apply_search_input_style()
        self.search_input.textChanged.connect(self._on_search_changed)
        self.search_input.textChanged.connect(self._update_search_background)
        bottom_layout.addWidget(self.search_input, 1)
        self.selection_manager.set_search_input(self.search_input)

        self.clear_search_btn = QPushButton("×")
        self.clear_search_btn.setFixedSize(24, 24)
        self.clear_search_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        self.clear_search_btn.setToolTip(self.tr("Clear search"))
        self.clear_search_btn.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self._apply_clear_search_btn_style()
        self.clear_search_btn.clicked.connect(self._clear_search)
        self.clear_search_btn.hide()
        bottom_layout.addWidget(self.clear_search_btn)

        self.menu_btn = QPushButton("⚙")
        self.menu_btn.setFixedSize(28, 28)
        self.menu_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        self.menu_btn.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self._apply_menu_btn_style()
        self.menu_btn.clicked.connect(self._show_main_menu)
        bottom_layout.addWidget(self.menu_btn)

        self._apply_time_filter_styles()

        self.left_layout.addWidget(self.bottom_bar)
        self.content_layout.addWidget(self.left_widget, 1)

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

        self._setup_mouse_tracking_recursive(self)

    def _get_menu_style(self):
        return ThemeStyleGenerator(self.current_theme).generate_menu_style()

    def _show_main_menu(self):
        if hasattr(self, "_settings_menu") and self._settings_menu is not None and self._settings_menu.isVisible():
            self._settings_menu.close()
            self._settings_menu = None
            return

        if hasattr(self, "_menu_close_time") and (perf_counter() - self._menu_close_time) < 0.3:
            return

        from ..panels.setting_panel import show_setting_menu

        self._settings_menu = show_setting_menu(
            parent=self,
            menu_style=self._get_menu_style(),
            tr=self.tr,
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

        def _on_menu_hide():
            self._menu_close_time = perf_counter()
            self._settings_menu = None

        self._settings_menu.aboutToHide.connect(_on_menu_hide)

    def _toggle_time_filter_bar(self, checked: bool):
        if not checked:
            self._clear_time_and_type_filters()
        self.time_filter_bar.setVisible(checked)
        self.time_filter_toggle_btn.setArrowType(Qt.ArrowType.DownArrow if checked else Qt.ArrowType.UpArrow)
        self._apply_time_filter_styles()

    def _apply_time_range_filter(self):
        start_text = self.start_date_edit.date().toString("yyyy/MM/dd")
        end_text = self.end_date_edit.date().toString("yyyy/MM/dd")
        range_text = f"{start_text}-{end_text}"
        ok = self.controller.set_time_range_text(range_text)
        self.start_date_edit.setProperty("hasError", not ok)
        self.end_date_edit.setProperty("hasError", not ok)
        self.start_date_edit.style().unpolish(self.start_date_edit)
        self.start_date_edit.style().polish(self.start_date_edit)
        self.end_date_edit.style().unpolish(self.end_date_edit)
        self.end_date_edit.style().polish(self.end_date_edit)
        self._apply_time_filter_styles()

    def _close_time_filter_bar(self):
        self.time_filter_toggle_btn.setChecked(False)
        self._toggle_time_filter_bar(False)

    def _clear_time_and_type_filters(self):
        today = QDate.currentDate()
        self.start_date_edit.setDate(today.addDays(-7))
        self.end_date_edit.setDate(today)
        self.start_date_edit.setProperty("hasError", False)
        self.end_date_edit.setProperty("hasError", False)
        if hasattr(self, "type_filter_btn"):
            self._update_type_filter_button(0)
        self.controller.clear_time_and_type_filters()

    def _toggle_paste_with_html(self, checked: bool):
        self.controller.set_paste_with_html(checked)

    def _toggle_auto_paste(self, checked: bool):
        self.controller.set_auto_paste(checked)

    def _toggle_move_to_top_on_paste(self, checked: bool):
        self.controller.set_move_to_top_on_paste(checked)

    def _toggle_show_metadata(self, checked: bool):
        self.config.set_clipboard_show_metadata(checked)
        if hasattr(self, "_item_delegate"):
            self._item_delegate.set_show_metadata(checked)
        self.controller.load_history()

    def _toggle_preserve_search(self, checked: bool):
        self.config.set_clipboard_preserve_search(checked)

    def _set_window_opacity(self, percent: int):
        self.window_opacity = percent
        self.config.set_clipboard_window_opacity(percent)
        if hasattr(self, "_item_delegate"):
            self._item_delegate.set_window_opacity(percent)
        self._apply_opacity()
        self._refresh_list()

    def _set_font_size(self, size: int):
        self.display_lines = size
        self.config.set_clipboard_font_size(size)
        if hasattr(self, "_item_delegate"):
            self._item_delegate.set_display_lines(size)
        self._refresh_list()

    def _set_theme(self, theme_name: str):
        get_theme_manager().set_theme(theme_name)

    def _on_font_size_changed(self, size: int):
        self.display_lines = size
        if hasattr(self, "_item_delegate"):
            self._item_delegate.set_display_lines(size)
        self._refresh_list()

    def _on_opacity_changed(self, percent: int):
        self.window_opacity = percent
        if hasattr(self, "_item_delegate"):
            self._item_delegate.set_window_opacity(percent)
        self._apply_opacity()
        self._refresh_list()

    def _setup_shortcuts(self):
        self._shortcut_handler = ClipboardShortcutHandler(self)

    def _load_history(self):
        self.controller.load_history()

    def _load_more_items(self):
        self.controller._load_more_items()

    def _refresh_list(self):
        PreviewPopup.instance().hide_preview()
        self.selection_manager.reset()

        self.list_widget.setUpdatesEnabled(False)
        self.list_widget.clear()

        show_metadata = self.config.get_clipboard_show_metadata()
        self._item_delegate.set_show_metadata(show_metadata)
        self._item_delegate.set_display_lines(self.display_lines)
        self._item_delegate.set_window_opacity(self.window_opacity)
        self._item_delegate.set_theme(self.current_theme)
        self._item_delegate.set_highlighted_id(None)

        for item in self.controller.current_items:
            list_item = QListWidgetItem()
            list_item.setData(ROLE_ITEM_ID, item.id)
            list_item.setData(ROLE_ITEM_DATA, item)
            self.list_widget.addItem(list_item)

        self.list_widget.setUpdatesEnabled(True)
        self.list_widget.scrollToTop()
        self.list_widget.update()

    def _on_load_completed(self):
        QTimer.singleShot(0, self._check_and_load_more_if_needed)

    def _check_and_load_more_if_needed(self):
        scrollbar = self.list_widget.verticalScrollBar()
        is_visible = scrollbar.isVisible()
        maximum = scrollbar.maximum()
        has_more = self.controller.has_more_items()

        if (not is_visible or maximum == 0) and has_more:
            if self._auto_fill_remaining <= 0:
                return
            self._auto_fill_remaining -= 1
            self.controller._load_more_items()

    def _append_items(self, items: List[ClipboardItem]):
        log_debug(f"📝 _append_items 被调用，准备追加 {len(items)} 条数据", "Clipboard")

        self.list_widget.setUpdatesEnabled(False)

        for item in items:
            list_item = QListWidgetItem()
            list_item.setData(ROLE_ITEM_ID, item.id)
            list_item.setData(ROLE_ITEM_DATA, item)
            self.list_widget.addItem(list_item)

        self.list_widget.setUpdatesEnabled(True)
        self.list_widget.update()

    def _on_scroll(self, value: int):
        scrollbar = self.list_widget.verticalScrollBar()
        maximum = scrollbar.maximum()
        self.controller.check_scroll_load(value, maximum)

    def _on_scrollbar_range_changed(self, min_val: int, max_val: int):
        _ = (min_val, max_val)
        self.list_widget.viewport().update()

    def _on_search_changed(self, text: str):
        _ = text
        if hasattr(self, "_search_timer"):
            self._search_timer.stop()
        else:
            self._search_timer = QTimer()
            self._search_timer.setSingleShot(True)
            self._search_timer.timeout.connect(lambda: self.controller.set_search_text(self.search_input.text()))

        self._search_timer.start(300)

    def _on_selection_changed(self):
        selected_items = self.list_widget.selectedItems()
        if selected_items:
            selected_id = selected_items[0].data(Qt.ItemDataRole.UserRole)
            self._set_highlighted_item(selected_id)
        else:
            self._set_highlighted_item(None)

    def _on_delegate_item_entered(self, list_item: QListWidgetItem):
        item_id = list_item.data(ROLE_ITEM_ID)
        if item_id is None:
            return
        self._set_highlighted_item(item_id)

        row = self.list_widget.row(list_item)
        if row >= 0:
            self.selection_manager.set_hovered_index(row)

        item_data = list_item.data(ROLE_ITEM_DATA)
        if item_data:
            popup = PreviewPopup.instance()
            pos = QCursor.pos()
            popup.show_preview(item_data, pos, delay_ms=5)

    def _set_highlighted_item(self, item_id: Optional[int]):
        if self._item_delegate._highlighted_id != item_id:
            self._item_delegate.set_highlighted_id(item_id)
            self.list_widget.viewport().update()

    def _update_search_background(self, text: str):
        if text.strip():
            self._apply_search_input_style(has_text=True)
            self.clear_search_btn.show()
        else:
            self._apply_search_input_style(has_text=False)
            self.clear_search_btn.hide()

    def _apply_search_input_style(self, has_text: bool = False):
        style = ThemeStyleGenerator(self.current_theme).generate_search_input_style(has_text)
        self.search_input.setStyleSheet(style)

    def _apply_clear_search_btn_style(self):
        style = ThemeStyleGenerator(self.current_theme).generate_clear_search_btn_style()
        self.clear_search_btn.setStyleSheet(style)

    def _apply_menu_btn_style(self):
        style = ThemeStyleGenerator(self.current_theme).generate_menu_btn_style()
        self.menu_btn.setStyleSheet(style)

    def _apply_time_filter_styles(self):
        generator = ThemeStyleGenerator(self.current_theme)
        if hasattr(self, "time_filter_toggle_btn"):
            self.time_filter_toggle_btn.setStyleSheet(generator.generate_filter_toggle_btn_style())
        if hasattr(self, "start_date_edit"):
            date_style = generator.generate_time_filter_date_edit_style()
            calendar_style = generator.generate_time_filter_calendar_style()
            self.start_date_edit.setStyleSheet(date_style)
            self.end_date_edit.setStyleSheet(date_style)
            self.start_date_edit.calendarWidget().setStyleSheet(calendar_style)
            self.end_date_edit.calendarWidget().setStyleSheet(calendar_style)
        if hasattr(self, "time_filter_separator"):
            self.time_filter_separator.setStyleSheet(
                f"background: transparent; border: none; color: {self.current_theme.colors.text_secondary};"
            )
        if hasattr(self, "apply_time_filter_btn"):
            self.apply_time_filter_btn.setStyleSheet(generator.generate_time_filter_action_btn_style(primary=True))
        if hasattr(self, "type_filter_btn"):
            self.type_filter_btn.setStyleSheet(generator.generate_time_filter_type_btn_style())
            self.type_filter_menu.setStyleSheet(generator.generate_menu_style())
        if hasattr(self, "clear_time_filter_btn"):
            self.clear_time_filter_btn.setStyleSheet(generator.generate_time_filter_action_btn_style(primary=True))

    def _clear_search(self):
        self.search_input.clear()
        self.search_input.setFocus()

    def _update_type_filter_button(self, index: int):
        self.type_filter_index = index
        self.type_filter_btn.setText(f"{self.type_filter_labels[index]} ▾")
        for action_index, action in enumerate(self.type_filter_actions):
            action.setChecked(action_index == index)

    def _show_type_filter_menu(self):
        self.type_filter_menu.exec(self.type_filter_btn.mapToGlobal(QPoint(0, self.type_filter_btn.height())))

    def _set_type_filter(self, index: int):
        self._update_type_filter_button(index)
        self.controller.set_content_type_filter(index)

    def _on_group_switched(self, group_id):
        self.group_bar.switch_to_group(group_id)
        is_quick_launch = False
        if group_id is not None:
            groups = self.controller.manager.get_groups()
            g = next((group for group in groups if group.id == group_id), None)
            is_quick_launch = g is not None and g.group_type == GroupType.FILE
        self._item_delegate.set_hide_file_icon(is_quick_launch)
        self.list_widget.update()

    def _on_sidebar_entered(self):
        self.selection_manager.clear_selection()
        self._set_highlighted_item(None)
        self.list_widget.setFocus()

    def _on_sidebar_exited(self, prev_item_id):
        self.list_widget.setFocus()
        if prev_item_id is not None:
            self.selection_manager.select_item_id(prev_item_id)
        else:
            self.selection_manager._move_selection(1)

    def _enter_sidebar_mode(self):
        prev_id = self.selection_manager.get_current_item_id()
        self.group_bar.enter_sidebar_mode(prev_id)

    def _on_top_group_switch(self, delta: int):
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

    def _connect_manage_dialog(self, dialog: ManageDialog):
        for sig, slot in [
            (dialog.group_added, self.group_bar.refresh_buttons),
            (dialog.data_changed, self._on_manage_data_changed),
        ]:
            sig.connect(slot, Qt.ConnectionType.UniqueConnection)

    def _set_group_bar_position(self, position: str):
        self.group_bar_position = position
        self.selection_manager.group_bar_position = position
        self.group_bar.set_position(position)
        self.config.set_clipboard_group_bar_position(position)
        self._apply_opacity()

    def _get_item_data(self, item_id: int) -> Optional[ClipboardItem]:
        for item in self.controller.current_items:
            if item.id == item_id:
                return item
        return None

    def _on_paste_item(self, item_id: int):
        if self.controller.current_group_id is not None:
            groups = self.controller.manager.get_groups()
            current_group = next((group for group in groups if group.id == self.controller.current_group_id), None)
            if current_group is not None and current_group.group_type == GroupType.FILE:
                self._open_file_item(item_id)
                return
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
        item = self.list_widget.itemAt(pos)
        if not item:
            return
        item_id = item.data(Qt.ItemDataRole.UserRole)
        if not item_id:
            return

        ctx = self.controller.build_context_menu_data(item_id)
        if ctx is None:
            return

        ClipboardItemContextMenu(
            parent=self,
            menu_style=self._get_menu_style(),
            translate=self.tr,
            action_handlers=self._get_item_context_menu_handlers(item_id),
            dynamic_handler_resolvers=(lambda action_key: self._resolve_item_context_menu_handler(item_id, action_key),),
        ).show(self.list_widget, pos, ctx)

    def _get_item_context_menu_handlers(self, item_id: int):
        return {
            "paste": lambda: self._paste_item_to_clipboard(item_id),
            "pin_image": lambda: self._create_pin_window(item_id),
            "save_image_as": lambda: self._save_image_as(item_id),
            "toggle_pin": lambda: self._toggle_pin(item_id),
            "open_file_location": lambda: self._open_file_location(item_id),
            "edit_item": lambda: self._edit_item(item_id),
            "move_item_up": lambda: self._move_item_order(item_id, -1),
            "move_item_down": lambda: self._move_item_order(item_id, 1),
            "delete_item": lambda: self._delete_item(item_id),
        }

    def _resolve_item_context_menu_handler(self, item_id: int, action_key: str):
        # "移出分组"
        if action_key == "remove_from_group":
            return lambda: self._move_item_to_group(item_id, None)

        # "移动到分组 xxx"
        prefix = "move_to_group_"
        if action_key.startswith(prefix):
            try:
                group_id = int(action_key[len(prefix):])
            except ValueError:
                return None
            return lambda: self._move_item_to_group(item_id, group_id)

        # "特殊粘贴" 子菜单项
        if action_key.startswith("special_paste_") or action_key.startswith("transform_"):
            return lambda: self._special_paste(item_id, action_key)

        # 文件项特殊粘贴子菜单项
        if action_key.startswith("file_paste_"):
            return lambda: self._file_special_paste(item_id, action_key)

        return None

    def _special_paste(self, item_id: int, action_key: str):
        """执行特殊粘贴：先记录前台窗口，再调用 controller 的加工粘贴。"""
        self.controller._previous_window_hwnd = get_foreground_window()
        self.controller.paste_transformed_text(item_id, action_key, on_close_callback=self.close)
        self.item_pasted.emit(item_id)

    def _file_special_paste(self, item_id: int, action_key: str):
        """执行文件项特殊粘贴。"""
        self.controller._previous_window_hwnd = get_foreground_window()
        self.controller.paste_file_text(item_id, action_key, on_close_callback=self.close)
        self.item_pasted.emit(item_id)

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

    def _open_file_item(self, item_id: int):
        import json
        import os
        from PySide6.QtCore import QTimer

        clipboard_item = self.controller.get_item(item_id)
        if clipboard_item is None or clipboard_item.content_type != "file":
            return
        try:
            data = json.loads(clipboard_item.content)
            files = data.get("files", [])
            if not files:
                return
            file_path = os.path.normpath(files[0])
            if os.path.exists(file_path):
                QTimer.singleShot(0, lambda p=file_path: os.startfile(p))
                QTimer.singleShot(50, self.hide)
        except Exception as e:
            from core.logger import log_warning

            log_warning(f"打开文件失败: {e}", "Clipboard")

    def _paste_item_to_clipboard(self, item_id: int):
        self.controller._previous_window_hwnd = get_foreground_window()
        if self.controller.paste_item(item_id, on_close_callback=self.close):
            self.item_pasted.emit(item_id)

    def _save_image_as(self, item_id: int):
        import os

        from PySide6.QtGui import QImage
        from PySide6.QtWidgets import QFileDialog
        from ui.dialogs import show_warning_dialog
        from core.save import SaveService

        clipboard_item = self.controller.get_item(item_id)
        if clipboard_item is None or clipboard_item.content_type != "image" or not clipboard_item.image_id:
            return

        image_data = self.manager.get_image_data(clipboard_item.image_id)
        if isinstance(image_data, list):
            image_data = bytes(image_data)
        if not image_data:
            show_warning_dialog(self, self.tr("Save Failed"), self.tr("Image data is unavailable."))
            return

        image = QImage()
        if not image.loadFromData(image_data):
            show_warning_dialog(self, self.tr("Save Failed"), self.tr("Failed to load image data."))
            return

        if clipboard_item.created_at:
            default_name = f"clipboard_image_{clipboard_item.created_at.strftime('%Y%m%d_%H%M%S')}.png"
        else:
            default_name = f"clipboard_image_{clipboard_item.id}.png"

        file_path, selected_filter = QFileDialog.getSaveFileName(
            self,
            self.tr("Save as"),
            default_name,
            self.tr("PNG Image (*.png);;JPEG Image (*.jpg *.jpeg);;Bitmap Image (*.bmp);;WebP Image (*.webp);;PDF (*.pdf)"),
        )
        if not file_path:
            return

        image_format = self._format_from_save_filter(file_path, selected_filter)
        if not os.path.splitext(file_path)[1]:
            file_path = f"{file_path}.{image_format.lower()}"

        save_service = SaveService()
        if not save_service.save_qimage_to_path(image, file_path, image_format=image_format):
            show_warning_dialog(self, self.tr("Save Failed"), self.tr("Failed to save image."))

    @staticmethod
    def _format_from_save_filter(file_path: str, selected_filter: str) -> str:
        ext = os.path.splitext(file_path)[1].lstrip(".")
        if ext:
            return ext.upper()
        # 从过滤器字符串中提取第一个扩展名，如 "JPEG (*.jpg *.jpeg)" → "jpg"
        match = re.search(r'\*\.(\w+)', selected_filter)
        if match:
            return match.group(1).upper()
        return "PNG"

    def _open_file_location(self, item_id: int):
        import json
        import os
        import subprocess
        from PySide6.QtCore import QTimer

        clipboard_item = self.controller.get_item(item_id)
        if clipboard_item is None or clipboard_item.content_type != "file":
            return
        try:
            data = json.loads(clipboard_item.content)
            files = data.get("files", [])
            if not files:
                return
            file_path = os.path.normpath(files[0])
            if os.path.isdir(file_path):
                QTimer.singleShot(0, lambda p=file_path: subprocess.Popen(["explorer", p]))
            elif os.path.exists(file_path):
                QTimer.singleShot(0, lambda p=file_path: subprocess.Popen(["explorer", "/select,", p]))
            elif os.path.exists(os.path.dirname(file_path)):
                QTimer.singleShot(0, lambda p=os.path.dirname(file_path): subprocess.Popen(["explorer", p]))
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
        if self._offscreen_warmup_in_progress:
            super().showEvent(event)
            return

        self.setWindowOpacity(0)

        if hasattr(self, "_shortcut_handler") and self._shortcut_handler:
            ShortcutManager.instance().register(self._shortcut_handler)

        self._fl_reset()
        self.selection_manager.reset()
        self.controller.on_window_show()

        t_show_start = perf_counter()
        super().showEvent(event)

        self.list_widget.setFocus()
        self._position_at_cursor()
        QTimer.singleShot(0, self.group_bar.refresh_buttons)
        QTimer.singleShot(0, self._reveal_window)
        t_show_end = perf_counter()
        log_debug(f"⏱️ 打开窗口耗时: {(t_show_end - t_show_start) * 1000:.1f} ms", "Clipboard")

    def _reveal_window(self):
        self.setWindowOpacity(1)

    def _position_at_cursor(self):
        cursor_pos = QCursor.pos()
        screen = QApplication.screenAt(cursor_pos)
        if screen:
            screen_geo = screen.availableGeometry()

            x = cursor_pos.x() + 10
            y = cursor_pos.y() + 10

            if x + self.width() > screen_geo.right():
                x = cursor_pos.x() - self.width() - 10

            if y + self.height() > screen_geo.bottom():
                y = cursor_pos.y() - self.height() - 10

            if x < screen_geo.left():
                x = screen_geo.left()
            if y < screen_geo.top():
                y = screen_geo.top()

            self.move(x, y)

    @safe_event
    def hideEvent(self, event):
        if self._offscreen_warmup_in_progress:
            super().hideEvent(event)
            return

        self._fl_reset()

        if not self.config.get_clipboard_preserve_search() and self.search_input.text():
            self.search_input.clear()

        super().hideEvent(event)
        self._save_window_geometry()
        PreviewPopup.instance().hide_preview()
        active_popup = QApplication.activePopupWidget()
        if active_popup is not None:
            active_popup.close()

    @safe_event
    def closeEvent(self, event):
        if hasattr(self, "_shortcut_handler") and self._shortcut_handler:
            ShortcutManager.instance().unregister(self._shortcut_handler)
        self._save_window_geometry()
        PreviewPopup.instance().hide_preview()
        active_popup = QApplication.activePopupWidget()
        if active_popup is not None:
            active_popup.close()
        self.closed.emit()
        super().closeEvent(event)

    @safe_event
    def changeEvent(self, event):
        super().changeEvent(event)
        if event.type() == QEvent.Type.ActivationChange and not self.isActiveWindow():
            QTimer.singleShot(100, self._check_and_hide)

    def _check_and_hide(self):
        if not self.isActiveWindow():
            self.hide()

    def _is_draggable_area(self, widget, local_pos: QPoint) -> bool:
        _ = local_pos
        bar = getattr(self, "group_bar", None)
        if bar and bar.bar_widget is not None and widget is bar.bar_widget:
            return True
        return False

    def _is_date_filter_widget(self, obj) -> bool:
        date_widgets = (
            getattr(self, "start_date_edit", None),
            getattr(self, "end_date_edit", None),
        )
        for widget in date_widgets:
            if widget is not None and (obj is widget or widget.isAncestorOf(obj)):
                return True
            if widget is not None:
                calendar = widget.calendarWidget()
                if calendar is not None and (obj is calendar or calendar.isAncestorOf(obj)):
                    return True
        return isinstance(obj, QCalendarWidget)

    @safe_event
    def eventFilter(self, obj, event):
        event_type = event.type()

        if event_type == QEvent.Type.Leave and obj is self.list_widget.viewport():
            self.selection_manager.clear_hovered_index()
            PreviewPopup.instance().hide_preview()
            selected_items = self.list_widget.selectedItems()
            if selected_items:
                selected_id = selected_items[0].data(Qt.ItemDataRole.UserRole)
                self._set_highlighted_item(selected_id)
            else:
                self._set_highlighted_item(None)

        if event_type == QEvent.Type.FocusIn and obj is self.list_widget:
            if not self.group_bar.sidebar_mode:
                self.group_bar.clear_sidebar_focus()

        if event_type == QEvent.Type.KeyPress:
            key = event.key()
            pos = getattr(self, "group_bar_position", "right")

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
                elif key == k_enter:
                    focus_widget = QApplication.focusWidget()
                    if focus_widget is self.list_widget or self.list_widget.isAncestorOf(focus_widget):
                        self._enter_sidebar_mode()
                        return True

        if event_type in (
            QEvent.Type.MouseButtonPress,
            QEvent.Type.MouseButtonRelease,
            QEvent.Type.MouseMove,
        ) and self._is_date_filter_widget(obj):
            return super().eventFilter(obj, event)

        if self._fl_handle_event(obj, event):
            return True

        return super().eventFilter(obj, event)

    @safe_event
    def leaveEvent(self, event):
        if not self._fl_is_dragging and not self._fl_resize_edge:
            self.unsetCursor()
        super().leaveEvent(event)

    @safe_event
    def keyPressEvent(self, event):
        key = event.key()

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
        super().resizeEvent(event)
        self.list_widget.viewport().update()
        if hasattr(self, "group_bar") and self.group_bar.bar_widget is not None:
            self.group_bar.refresh_buttons()


__all__ = ["ClipboardShortcutHandler", "ClipboardWindow"]

__all__ = ["ClipboardWindow"]
