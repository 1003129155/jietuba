# -*- coding: utf-8 -*-
"""
设置对话框主类 — Fluent 风格

负责：导航栏、内容堆栈、底部按钮、accept/refresh/reset 逻辑。
各个页面分别位于 page_*.py 模块中。
"""
import os
import subprocess
import sys

from PySide6.QtWidgets import (
    QDialog, QVBoxLayout, QHBoxLayout, QLabel, QPushButton,
    QStackedWidget, QWidget, QDialogButtonBox,
    QFrame, QFileDialog,
)
from PySide6.QtCore import Qt, Signal
from ui.dialogs import show_info_dialog
from PySide6.QtGui import QFont, QIcon, QColor

from qfluentwidgets import (
    NavigationInterface, NavigationItemPosition,
    FluentIcon, BodyLabel,
    PushButton as FluentPushButton,
    PrimaryPushButton, TransparentPushButton,
)
from qframelesswindow import FramelessDialog
from qfluentwidgets.window.fluent_window import FluentTitleBar

from core import log_info, safe_event
from core.logger import log_exception
from core.constants import CSS_FONT_FAMILY, DEFAULT_FONT_FAMILY

# 页面创建函数
from .page_hotkey import create_hotkey_page
from .page_capture import create_capture_page
from .page_clipboard import create_clipboard_page
from .page_translation import create_translation_page
from .page_log import create_log_page, refresh_latest_log_label
from .page_misc import create_misc_page
from .page_appearance import create_appearance_page
from .page_developer import create_developer_page
from .page_about import create_about_page
from .components import (
    adjust_button_width, theme_surface_color, theme_sidebar_color,
    theme_border_color, theme_input_background, theme_popup_background,
    theme_popup_hover_background, theme_text_style, theme_caption_style,
    theme_menu_style, theme_color,
)


class SettingsDialog(FramelessDialog):
    """现代化设置对话框 - Fluent 风格（无系统标题栏）"""

    wizard_requested = Signal()

    def __init__(self, config_manager=None, current_hotkey="ctrl+shift+a", parent=None):
        super().__init__(parent)
        self.config_manager = config_manager
        self.current_hotkey = current_hotkey
        self.main_window = parent
        self._skip_unsaved_close_prompt = False

        if self.config_manager is None:
            from .mock_config import MockConfig
            self.config_manager = MockConfig()

        # 自定义 Fluent 标题栏（在 setWindowTitle 之前，以接收信号）
        self._setup_titlebar()

        self.setWindowTitle("jietuba")
        self.resize(860, 620)
        self.setFont(QFont(DEFAULT_FONT_FAMILY, 10))
        self.setObjectName("SettingsDialog")

        self._setup_ui()

    def _setup_titlebar(self):
        """用 FluentTitleBar 替换默认标题栏"""
        title_bar = FluentTitleBar(self)
        self.setTitleBar(title_bar)
        title_bar.iconLabel.hide()
        title_bar.titleLabel.hide()
        title_bar.setFixedHeight(title_bar.buttonLayout.sizeHint().height())
        title_bar.hBoxLayout.setContentsMargins(0, 0, 0, 0)
        title_bar.maxBtn.hide()
        title_bar.setDoubleClickEnabled(False)

        # 设置窗口图标
        try:
            from core.resource_manager import ResourceManager
            icon_path = ResourceManager.get_resource_path("svg/托盘.svg")
            if os.path.exists(icon_path):
                self.setWindowIcon(QIcon(icon_path))
        except Exception as e:
            log_exception(e, "设置窗口图标")

    # ================================================================
    # UI 构建
    # ================================================================

    def _setup_ui(self):
        sidebar_width = 204
        nav_width = 188
        title_bar_height = self.titleBar.height() if getattr(self, 'titleBar', None) else 32

        main_layout = QHBoxLayout(self)
        main_layout.setContentsMargins(8, title_bar_height, 8, 8)
        main_layout.setSpacing(0)

        # 1. 左侧面板
        left_panel = QWidget()
        left_panel.setObjectName("SettingsLeftPanel")
        left_panel.setFixedWidth(sidebar_width)
        left_v = QVBoxLayout(left_panel)
        left_v.setContentsMargins(10, 6, 6, 10)
        left_v.setSpacing(8)

        # logo 区
        logo_area = QWidget()
        logo_area.setFixedHeight(78)
        logo_layout = QHBoxLayout(logo_area)
        logo_layout.setContentsMargins(12, 10, 12, 10)
        logo_layout.setSpacing(10)
        logo_layout.setAlignment(Qt.AlignmentFlag.AlignVCenter)

        logo_icon_lbl = QLabel()
        logo_icon_lbl.setFixedSize(36, 36)
        logo_icon_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)

        try:
            from core.resource_manager import ResourceManager
            icon_path = ResourceManager.get_resource_path("svg/托盘.svg")
            if os.path.exists(icon_path):
                pm = QIcon(icon_path).pixmap(36, 36)
                logo_icon_lbl.setPixmap(pm)
        except Exception as e:
            log_exception(e, "加载 Logo 图标")
        logo_layout.addWidget(logo_icon_lbl, 0, Qt.AlignmentFlag.AlignVCenter)

        text_box = QVBoxLayout()
        text_box.setContentsMargins(0, 0, 0, 0)
        text_box.setSpacing(2)
        app_name_lbl = BodyLabel(self.tr("jietuba"))
        app_name_lbl.setStyleSheet("font-size: 15px; font-weight: 600;")
        app_desc_lbl = QLabel(self.tr("Settings"))
        app_desc_lbl.setStyleSheet("font-size: 12px; color: rgba(120, 120, 120, 0.9); background: transparent;")
        text_box.addWidget(app_name_lbl)
        text_box.addWidget(app_desc_lbl)
        logo_layout.addLayout(text_box, 1)
        left_v.addWidget(logo_area)

        # logo 右键菜单 → 开发者选项
        logo_area.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        logo_area.customContextMenuRequested.connect(self._show_logo_context_menu)

        self.nav_list = self._create_navigation(left_panel)
        left_v.addWidget(self.nav_list, 1)
        main_layout.addWidget(left_panel)

        # 2. 右侧内容区
        right_area = QWidget()
        right_area.setObjectName("SettingsRightArea")
        right_layout = QVBoxLayout(right_area)
        right_layout.setContentsMargins(26, 12, 26, 18)
        right_layout.setSpacing(14)

        self.content_title = QLabel(self.tr("Shortcut Settings"))
        self.content_title.setStyleSheet("font-size: 22px; font-weight: 700; margin-bottom: 6px; background-color: transparent;")
        self.content_title.hide()

        self.content_stack = QStackedWidget()
        self.content_stack.addWidget(create_hotkey_page(self))           # 0
        self.content_stack.addWidget(create_capture_page(self))          # 1
        self.content_stack.addWidget(create_clipboard_page(self))        # 2
        self.content_stack.addWidget(create_appearance_page(self))       # 3
        self.content_stack.addWidget(create_translation_page(self))      # 4
        self.content_stack.addWidget(create_log_page(self))              # 5
        self.content_stack.addWidget(create_misc_page(self))             # 6
        self.content_stack.addWidget(create_developer_page(self))        # 7
        self.content_stack.addWidget(create_about_page(self))            # 8

        right_layout.addWidget(self.content_stack)
        right_layout.setStretchFactor(self.content_stack, 1)
        right_layout.addLayout(self._create_button_area())
        main_layout.addWidget(right_area, 1)

        self._apply_dialog_stylesheet()

        self._set_current_nav("shortcuts")

    def _create_navigation(self, parent=None):
        """创建左侧导航栏"""
        nav = NavigationInterface(parent=parent, showMenuButton=False, showReturnButton=False, collapsible=False)
        nav.setObjectName("SettingsNavigation")
        nav.setExpandWidth(188)
        nav.setMinimumExpandWidth(0)
        nav.expand(useAni=False)
        nav.setMinimumWidth(188)
        nav.setMaximumWidth(196)

        self._nav_items = [
            ("shortcuts", FluentIcon.COMMAND_PROMPT, self.tr("Shortcuts"), 0, NavigationItemPosition.TOP),
            ("capture", FluentIcon.CAMERA, self.tr("Capture Settings"), 1, NavigationItemPosition.TOP),
            ("clipboard", FluentIcon.PASTE, self.tr("Clipboard"), 2, NavigationItemPosition.TOP),
            ("appearance", FluentIcon.BRUSH, self.tr("Appearance"), 3, NavigationItemPosition.TOP),
            ("translation", FluentIcon.LANGUAGE, self.tr("Translation"), 4, NavigationItemPosition.TOP),
            ("log", FluentIcon.HISTORY, self.tr("Log Settings"), 5, NavigationItemPosition.TOP),
            ("other", FluentIcon.APPLICATION, self.tr("Other"), 6, NavigationItemPosition.TOP),
            ("about", FluentIcon.INFO, self.tr("About"), 8, NavigationItemPosition.BOTTOM),
        ]

        for route_key, icon, text, stack_index, position in self._nav_items:
            nav.addItem(
                routeKey=route_key,
                icon=icon,
                text=text,
                onClick=lambda checked=False, idx=stack_index, rk=route_key: self._on_nav_changed(idx, rk),
                position=position,
                tooltip=text,
            )

        return nav

    def _set_current_nav(self, route_key: str):
        if hasattr(self, 'nav_list') and self.nav_list is not None:
            self.nav_list.setCurrentItem(route_key)

    # ================================================================
    # 辅助方法
    # ================================================================

    def _create_toggle_row(self, title, desc, checked_state, toggle_obj):
        """创建一个标准的一行设置：左字右开关"""
        row = QHBoxLayout()
        text_layout = QVBoxLayout()
        lbl_title = QLabel(title)
        lbl_title.setStyleSheet(theme_text_style(13))
        text_layout.addWidget(lbl_title)
        if desc:
            lbl_desc = QLabel(desc)
            lbl_desc.setStyleSheet(theme_caption_style(12))
            text_layout.addWidget(lbl_desc)
        row.addLayout(text_layout)
        row.addStretch()
        toggle_obj.setChecked(checked_state)
        row.addWidget(toggle_obj)
        return row

    def _get_input_style(self):
        input_bg = theme_input_background()
        popup_bg = theme_popup_background()
        popup_hover = theme_popup_hover_background()
        text_color = theme_color("#202020", "#F3F3F3")
        border_color = theme_color("#D9DDE3", "#3A3D43")
        focus_bg = theme_color("#FFFFFF", "#25272B")
        arrow_color = theme_color("#666666", "#D0D0D0")
        return f"""
            QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox {{
                border: 1px solid {border_color}; border-radius: 4px;
                padding: 4px 8px; background-color: {input_bg};
                color: {text_color}; font-family: {CSS_FONT_FAMILY};
                font-size: 12px;
            }}
            QLineEdit:focus, QSpinBox:focus {{
                border: 1px solid #07C160; background-color: {focus_bg};
            }}
            QSpinBox, QDoubleSpinBox {{ padding-right: 24px; }}
            QSpinBox::up-button, QDoubleSpinBox::up-button {{
                subcontrol-origin: border; subcontrol-position: top right;
                width: 20px; border-left: 1px solid {border_color};
                border-bottom: 1px solid {border_color}; border-top-right-radius: 4px;
                background: {input_bg};
            }}
            QSpinBox::up-button:hover, QDoubleSpinBox::up-button:hover {{ background: {popup_hover}; }}
            QSpinBox::up-button:pressed, QDoubleSpinBox::up-button:pressed {{ background: #C8E6C9; }}
            QSpinBox::up-arrow, QDoubleSpinBox::up-arrow {{
                image: none; border-left: 4px solid transparent;
                border-right: 4px solid transparent; border-bottom: 6px solid {arrow_color};
                width: 0; height: 0;
            }}
            QSpinBox::down-button, QDoubleSpinBox::down-button {{
                subcontrol-origin: border; subcontrol-position: bottom right;
                width: 20px; border-left: 1px solid {border_color};
                border-bottom-right-radius: 4px; background: {input_bg};
            }}
            QSpinBox::down-button:hover, QDoubleSpinBox::down-button:hover {{ background: {popup_hover}; }}
            QSpinBox::down-button:pressed, QDoubleSpinBox::down-button:pressed {{ background: #C8E6C9; }}
            QSpinBox::down-arrow, QDoubleSpinBox::down-arrow {{
                image: none; border-left: 4px solid transparent;
                border-right: 4px solid transparent; border-top: 6px solid {arrow_color};
                width: 0; height: 0;
            }}
            QComboBox::drop-down {{
                subcontrol-origin: padding; subcontrol-position: top right;
                width: 20px; border-left: 1px solid {border_color};
                border-top-right-radius: 4px; border-bottom-right-radius: 4px;
                background: {input_bg};
            }}
            QComboBox::down-arrow {{
                image: none; border-left: 4px solid transparent;
                border-right: 4px solid transparent; border-top: 6px solid {arrow_color};
                width: 0; height: 0; margin-right: 6px;
            }}
            QComboBox QAbstractItemView {{
                border: 1px solid {border_color}; background: {popup_bg};
                selection-background-color: #07C160; selection-color: white;
                font-family: {CSS_FONT_FAMILY};
                font-size: 12px; color: {text_color}; outline: none;
            }}
            QComboBox QAbstractItemView::item {{
                padding: 6px 8px; min-height: 24px; color: {text_color}; background: {popup_bg};
            }}
            QComboBox QAbstractItemView::item:hover {{
                background-color: {popup_hover}; color: {text_color};
            }}
            QComboBox QAbstractItemView::item:selected {{
                background-color: #07C160; color: white;
            }}
        """

    # ================================================================
    # 导航 & 开发者入口
    # ================================================================

    def _on_nav_changed(self, stack_index, route_key=None):
        title_map = {
            0: self.tr("Shortcut Settings"),
            1: self.tr("Capture Settings"),
            2: self.tr("Clipboard Settings"),
            3: self.tr("Appearance Settings"),
            4: self.tr("Translation Settings"),
            5: self.tr("Log Settings"),
            6: self.tr("Other Settings"),
            8: self.tr("Software Information"),
        }

        if stack_index in title_map:
            self.content_title.setText(title_map[stack_index])
            self.content_stack.setCurrentIndex(stack_index)
            if route_key:
                self._set_current_nav(route_key)

        if stack_index == 2:
            self._refresh_clipboard_size()

    def _refresh_clipboard_size(self, delay_ms: int = 0):
        if not hasattr(self, '_clipboard_size_label') or not hasattr(self, '_calc_clipboard_storage_size'):
            return
        if delay_ms > 0:
            from PySide6.QtCore import QTimer
            QTimer.singleShot(delay_ms, self._refresh_clipboard_size)
            return
        size_str = self._calc_clipboard_storage_size()
        self._clipboard_size_label.setText(size_str if size_str else "—")

    def _show_logo_context_menu(self, pos):
        from PySide6.QtWidgets import QMenu
        menu = QMenu(self)
        menu.setStyleSheet(theme_menu_style())
        action_dev = menu.addAction(self.tr("Developer Options"))
        action = menu.exec(self.mapToGlobal(pos) if pos else self.cursor().pos())
        if action == action_dev:
            self._open_developer_page()

    def _open_developer_page(self):
        self.content_stack.setCurrentIndex(7)
        self.content_title.setText(self.tr("Developer Options"))

    def _open_welcome_wizard(self):
        self.wizard_requested.emit()

    # ================================================================
    # 文件/目录操作
    # ================================================================

    def _change_save_dir(self):
        new_dir = QFileDialog.getExistingDirectory(self, self.tr("Select Screenshot Save Folder"), self.config_manager.get_screenshot_save_path())
        if new_dir:
            self.save_path_lbl.setText(new_dir)

    def _open_save_dir(self):
        path = self.config_manager.get_screenshot_save_path()
        if not os.path.exists(path):
            os.makedirs(path, exist_ok=True)
        if sys.platform == "win32":
            os.startfile(path)
        elif sys.platform == "darwin":
            subprocess.run(["open", path], check=False)
        else:
            subprocess.run(["xdg-open", path], check=False)

    def _change_log_dir(self):
        new_dir = QFileDialog.getExistingDirectory(self, self.tr("Select Log Save Folder"), self.config_manager.get_log_dir())
        if new_dir:
            self.path_lbl.setText(new_dir)

    def _open_log_dir(self):
        path = self.config_manager.get_log_dir()
        if not os.path.exists(path):
            os.makedirs(path, exist_ok=True)
        if sys.platform == "win32":
            os.startfile(path)
        elif sys.platform == "darwin":
            subprocess.run(["open", path], check=False)
        else:
            subprocess.run(["xdg-open", path], check=False)

    # ================================================================
    # 底部按钮
    # ================================================================

    def _create_button_area(self):
        layout = QHBoxLayout()
        layout.setSpacing(12)

        reset_btn = TransparentPushButton(self.tr("Reset This Page"))
        reset_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        reset_btn.setIcon(FluentIcon.DELETE)
        reset_btn.clicked.connect(self._reset_current_page)

        cancel_btn = FluentPushButton(self.tr("Cancel"))
        cancel_btn.setFixedHeight(36)
        cancel_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        cancel_btn.setIcon(FluentIcon.CLOSE)
        adjust_button_width(cancel_btn, min_width=112)
        cancel_btn.clicked.connect(self.reject)

        ok_btn = PrimaryPushButton(self.tr("Apply"))
        ok_btn.setFixedHeight(36)
        ok_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        ok_btn.setIcon(FluentIcon.SAVE)
        adjust_button_width(ok_btn, min_width=112)
        ok_btn.clicked.connect(self.accept)

        layout.addWidget(reset_btn)
        layout.addStretch()
        layout.addWidget(cancel_btn)
        layout.addWidget(ok_btn)
        return layout

    # ================================================================
    # 重置页面
    # ================================================================

    def _reset_current_page(self):
        current_index = self.content_stack.currentIndex()
        if current_index == 0:
            self._reset_hotkey_page()
        elif current_index == 1:
            self._reset_screenshot_settings_page()
        elif current_index == 2:
            self._reset_clipboard_page()
        elif current_index == 3:
            self._reset_appearance_page()
        elif current_index == 4:
            self._reset_translation_page()
        elif current_index == 5:
            self._reset_log_page()
        elif current_index == 6:
            self._reset_misc_page()
        elif current_index == 7:
            self._reset_long_screenshot_page()
        elif current_index == 8:
            pass

    def _reset_hotkey_page(self):
        defaults = self.config_manager.APP_DEFAULT_SETTINGS
        self.hotkey_input.setText(defaults["hotkey"])
        if hasattr(self, 'hotkey_input_2'):
            self.hotkey_input_2.setText(defaults["hotkey_2"])
        if hasattr(self, 'clipboard_hotkey_edit'):
            self.clipboard_hotkey_edit.setText(defaults["clipboard_hotkey"])
        if hasattr(self, 'clipboard_hotkey_edit_2'):
            self.clipboard_hotkey_edit_2.setText(defaults["clipboard_hotkey_2"])
        # 应用内快捷键
        if hasattr(self, '_inapp_edits'):
            for cfg_key, edit in self._inapp_edits.items():
                edit.setText(defaults.get(cfg_key, ""))
        if hasattr(self, 'cursor_move_combo'):
            idx = self.cursor_move_combo.findData(defaults["inapp_cursor_move_mode"])
            if idx >= 0:
                self.cursor_move_combo.setCurrentIndex(idx)

    def _reset_long_screenshot_page(self):
        defaults = self.config_manager.APP_DEFAULT_SETTINGS
        if hasattr(self, 'engine_combo'):
            index = self.engine_combo.findData(defaults["long_stitch_engine"])
            if index >= 0:
                self.engine_combo.setCurrentIndex(index)
        if hasattr(self, 'cooldown_spinbox'):
            self.cooldown_spinbox.setValue(defaults["scroll_cooldown"])

    def _reset_appearance_page(self):
        """重置外观设置页面"""
        from .page_appearance import _update_color_btn
        defaults = self.config_manager.APP_DEFAULT_SETTINGS
        if hasattr(self, '_appearance_theme_color'):
            self._appearance_theme_color = QColor(defaults["theme_color"])
            _update_color_btn(self._theme_color_btn, self._appearance_theme_color)
        if hasattr(self, '_appearance_mask_color'):
            self._appearance_mask_color = QColor(
                defaults["mask_color_r"], defaults["mask_color_g"],
                defaults["mask_color_b"]
            )
            _update_color_btn(self._mask_color_btn, self._appearance_mask_color)

    def _reset_screenshot_settings_page(self):
        defaults = self.config_manager.APP_DEFAULT_SETTINGS
        if hasattr(self, 'smart_toggle'):
            self.smart_toggle.setChecked(defaults["smart_selection"])
        if hasattr(self, 'save_toggle'):
            self.save_toggle.setChecked(defaults["screenshot_save_enabled"])
        if hasattr(self, 'save_path_lbl'):
            self.save_path_lbl.setText(defaults["screenshot_save_path"])
        if hasattr(self, 'screenshot_format_combo'):
            idx = {"PNG": 0, "JPG": 1, "BMP": 2, "WEBP": 3}.get(defaults["screenshot_format"].upper(), 0)
            self.screenshot_format_combo.setCurrentIndex(idx)
        if hasattr(self, 'ocr_enable_toggle'):
            self.ocr_enable_toggle.setChecked(defaults["ocr_enabled"])
        if hasattr(self, 'ocr_engine_combo'):
            index = self.ocr_engine_combo.findData(defaults["ocr_engine"])
            if index >= 0:
                self.ocr_engine_combo.setCurrentIndex(index)

    def _reset_log_page(self):
        defaults = self.config_manager.APP_DEFAULT_SETTINGS
        if hasattr(self, 'log_toggle'):
            self.log_toggle.setChecked(defaults["log_enabled"])
        if hasattr(self, 'log_level_combo'):
            self.log_level_combo.setCurrentText(defaults["log_level"])
        if hasattr(self, 'log_retention_spinbox'):
            self.log_retention_spinbox.setValue(defaults["log_retention_days"])
        if hasattr(self, 'path_lbl'):
            self.path_lbl.setText(defaults["log_dir"])

    def _reset_misc_page(self):
        defaults = self.config_manager.APP_DEFAULT_SETTINGS
        if hasattr(self, 'autostart_toggle'):
            self.autostart_toggle.setChecked(False)
        if hasattr(self, 'show_main_window_toggle'):
            self.show_main_window_toggle.setChecked(defaults["show_main_window"])
        if hasattr(self, 'pin_auto_toolbar_toggle'):
            self.pin_auto_toolbar_toggle.setChecked(defaults["pin_auto_toolbar"])
        if hasattr(self, 'magnifier_color_format_combo'):
            index = self.magnifier_color_format_combo.findData(defaults.get("magnifier_color_copy_format", "rgb_hex"))
            if index >= 0:
                self.magnifier_color_format_combo.setCurrentIndex(index)

    def _reset_translation_page(self):
        defaults = self.config_manager.APP_DEFAULT_SETTINGS
        if hasattr(self, 'deepl_api_key_input'):
            self.deepl_api_key_input.setText(defaults["deepl_api_key"])
        if hasattr(self, 'deepl_pro_toggle'):
            self.deepl_pro_toggle.setChecked(defaults["deepl_use_pro"])
        if hasattr(self, 'translation_target_combo'):
            index = self.translation_target_combo.findData(defaults["translation_target_lang"])
            if index >= 0:
                self.translation_target_combo.setCurrentIndex(index)
        if hasattr(self, 'split_sentences_toggle'):
            self.split_sentences_toggle.setChecked(defaults["translation_split_sentences"])
        if hasattr(self, 'preserve_formatting_toggle'):
            self.preserve_formatting_toggle.setChecked(defaults["translation_preserve_formatting"])

    def _reset_clipboard_page(self):
        defaults = self.config_manager.APP_DEFAULT_SETTINGS
        if hasattr(self, 'clipboard_enabled_toggle'):
            self.clipboard_enabled_toggle.setChecked(defaults["clipboard_enabled"])
        if hasattr(self, 'clipboard_auto_paste_toggle'):
            self.clipboard_auto_paste_toggle.setChecked(defaults["clipboard_auto_paste"])
        if hasattr(self, 'clipboard_history_limit_spin'):
            self.clipboard_history_limit_spin.setValue(defaults["clipboard_history_limit"])

    # ================================================================
    # 保存（accept）
    # ================================================================

    def accept(self):
        """保存所有设置"""
        # 防止保存过程中（比如语言切换触发的窗口重建）触发未保存确认弹窗
        self._skip_unsaved_close_prompt = True

        # 0. 快捷键
        self.config_manager.set_hotkey(self.hotkey_input.text().strip())
        if hasattr(self, 'hotkey_input_2'):
            self.config_manager.set_hotkey_2(self.hotkey_input_2.text().strip())

        # 1. 智能选区
        if hasattr(self, 'smart_toggle'):
            self.config_manager.set_smart_selection(self.smart_toggle.isChecked())

        # 2. 日志设置
        if hasattr(self, 'log_toggle'):
            log_enabled = self.log_toggle.isChecked()
            self.config_manager.set_log_enabled(log_enabled)

            if hasattr(self, 'log_level_combo'):
                log_level = self.log_level_combo.currentText()
                self.config_manager.set_log_level(log_level)
                from core.logger import get_logger, LogLevel
                logger = get_logger()
                level_map = {
                    "DEBUG": LogLevel.DEBUG, "INFO": LogLevel.INFO,
                    "WARNING": LogLevel.WARNING, "ERROR": LogLevel.ERROR,
                }
                if log_level in level_map:
                    logger.set_level(level_map[log_level])
                    logger.set_console_level(level_map[log_level])

            if hasattr(self, 'log_retention_spinbox'):
                retention_days = self.log_retention_spinbox.value()
                old_retention = self.config_manager.get_log_retention_days()
                self.config_manager.set_log_retention_days(retention_days)
                if retention_days > 0 and retention_days < old_retention:
                    from core.logger import cleanup_old_logs
                    log_dir = self.config_manager.get_log_dir()
                    cleanup_old_logs(log_dir, retention_days)

            if hasattr(self, 'path_lbl'):
                old_log_dir = self.config_manager.get_log_dir()
                new_log_dir = self.path_lbl.text()
                self.config_manager.set_log_dir(new_log_dir)
                from core.logger import get_logger
                logger = get_logger()
                logger.set_enabled(log_enabled)
                if new_log_dir != old_log_dir:
                    logger.set_log_dir(new_log_dir)
                    show_info_dialog(
                        self, self.tr("Log"),
                        self.tr("Log save location changed.") + "\n" + self.tr("*Changes will fully take effect after restart.")
                    )
                self._refresh_latest_log_label()

        # 3. 截图保存
        if hasattr(self, 'save_toggle'):
            self.config_manager.set_screenshot_save_enabled(self.save_toggle.isChecked())
        if hasattr(self, 'save_path_lbl'):
            self.config_manager.set_screenshot_save_path(self.save_path_lbl.text())
        if hasattr(self, 'screenshot_format_combo'):
            self.config_manager.set_screenshot_format(self.screenshot_format_combo.currentData())

        # 4. OCR
        if hasattr(self, 'ocr_enable_toggle'):
            self.config_manager.set_ocr_enabled(self.ocr_enable_toggle.isChecked())
        if hasattr(self, 'ocr_engine_combo'):
            self.config_manager.set_ocr_engine(self.ocr_engine_combo.currentData())
        if hasattr(self, 'ocr_grayscale_toggle'):
            self.config_manager.set_ocr_grayscale_enabled(self.ocr_grayscale_toggle.isChecked())
        if hasattr(self, 'ocr_upscale_toggle'):
            self.config_manager.set_ocr_upscale_enabled(self.ocr_upscale_toggle.isChecked())
        if hasattr(self, 'ocr_scale_spinbox'):
            self.config_manager.set_ocr_upscale_factor(self.ocr_scale_spinbox.value())

        # 5. 翻译
        if hasattr(self, 'deepl_api_key_input'):
            self.config_manager.set_deepl_api_key(self.deepl_api_key_input.text().strip())
        if hasattr(self, 'deepl_pro_toggle'):
            self.config_manager.set_deepl_use_pro(self.deepl_pro_toggle.isChecked())
        if hasattr(self, 'translation_target_combo'):
            self.config_manager.set_translation_target_lang(self.translation_target_combo.currentData())
        if hasattr(self, 'split_sentences_toggle'):
            self.config_manager.set_translation_split_sentences(self.split_sentences_toggle.isChecked())
        if hasattr(self, 'preserve_formatting_toggle'):
            self.config_manager.set_translation_preserve_formatting(self.preserve_formatting_toggle.isChecked())

        # 6. 杂项
        if hasattr(self, 'autostart_toggle'):
            from ..welcome.page6_finish import FinishPage as _FP
            _FP._set_autostart(self.autostart_toggle.isChecked())
        if hasattr(self, 'show_main_window_toggle'):
            self.config_manager.set_show_main_window(self.show_main_window_toggle.isChecked())
        if hasattr(self, 'pin_auto_toolbar_toggle'):
            self.config_manager.set_pin_auto_toolbar(self.pin_auto_toolbar_toggle.isChecked())
        if hasattr(self, 'magnifier_color_format_combo'):
            self.config_manager.set_app_setting(
                "magnifier_color_copy_format",
                self.magnifier_color_format_combo.currentData()
            )

        # 界面语言
        if hasattr(self, 'language_combo'):
            new_lang = self.language_combo.currentData()
            old_lang = self.config_manager.get_app_setting("language", "ja")
            self.config_manager.qsettings.setValue("app/language", new_lang)
            if new_lang != old_lang:
                from core.i18n import I18nManager
                I18nManager.load_language(new_lang)

        # 7. 剪贴板
        if hasattr(self, 'clipboard_enabled_toggle'):
            self.config_manager.set_clipboard_enabled(self.clipboard_enabled_toggle.isChecked())
        if hasattr(self, 'clipboard_history_limit_spin'):
            self.config_manager.set_clipboard_history_limit(self.clipboard_history_limit_spin.value())
        if hasattr(self, 'clipboard_hotkey_edit'):
            self.config_manager.set_clipboard_hotkey(self.clipboard_hotkey_edit.text().strip())
        if hasattr(self, 'clipboard_hotkey_edit_2'):
            self.config_manager.set_clipboard_hotkey_2(self.clipboard_hotkey_edit_2.text().strip())

        # 7.5 应用内快捷键
        if hasattr(self, '_inapp_edits'):
            for cfg_key, edit in self._inapp_edits.items():
                val = edit.text().strip()
                if val and not val.endswith("+"):
                    self.config_manager.set_inapp_shortcut(cfg_key, val)
            # 通知钉图快捷键 handler 重新加载绑定
            try:
                from pin.pin_shortcut import PinShortcutController
                pin_ctrl = PinShortcutController._instance
                if pin_ctrl is not None:
                    pin_ctrl._edit_handler.reload_bindings()
                    pin_ctrl._normal_handler.reload_bindings()
            except Exception as e:
                log_exception(e, "重载 Pin 快捷键绑定")
        if hasattr(self, 'cursor_move_combo'):
            self.config_manager.set_inapp_cursor_move_mode(
                self.cursor_move_combo.currentData()
            )

        # 8. 长截图/开发者
        if hasattr(self, 'engine_combo'):
            self.config_manager.set_long_stitch_engine(self.engine_combo.currentData())
        if hasattr(self, 'cooldown_spinbox'):
            self.config_manager.settings.setValue('screenshot/scroll_cooldown', self.cooldown_spinbox.value())

        # 9. 预加载开关（重启生效）
        if hasattr(self, 'preload_fonts_toggle'):
            self.config_manager.set_app_setting("preload_fonts", self.preload_fonts_toggle.isChecked())
        if hasattr(self, 'preload_screenshot_toggle'):
            self.config_manager.set_app_setting("preload_screenshot", self.preload_screenshot_toggle.isChecked())
        if hasattr(self, 'preload_toolbar_toggle'):
            self.config_manager.set_app_setting("preload_toolbar", self.preload_toolbar_toggle.isChecked())
        if hasattr(self, 'preload_ocr_toggle'):
            self.config_manager.set_app_setting("preload_ocr", self.preload_ocr_toggle.isChecked())
        if hasattr(self, 'preload_settings_toggle'):
            self.config_manager.set_app_setting("preload_settings", self.preload_settings_toggle.isChecked())
        if hasattr(self, 'preload_clipboard_toggle'):
            self.config_manager.set_app_setting("preload_clipboard", self.preload_clipboard_toggle.isChecked())

        # 10. 截图信息面板行为
        if hasattr(self, 'info_hide_on_drag_toggle'):
            self.config_manager.set_app_setting("screenshot_info_hide_on_drag", self.info_hide_on_drag_toggle.isChecked())

        # 11. 外观设置（主题色、遮罩色）
        from core.theme import get_theme
        theme = get_theme()
        if hasattr(self, '_appearance_theme_color'):
            theme.set_theme_color(self._appearance_theme_color)
        if hasattr(self, '_appearance_mask_color'):
            theme.set_mask_color(self._appearance_mask_color)

        log_info("すべての設定を保存しました", "Settings")
        self._settings_snapshot = self._snapshot_settings()
        self._skip_unsaved_close_prompt = True
        try:
            super().accept()
        finally:
            self._skip_unsaved_close_prompt = False

    # ================================================================
    # showEvent / refresh
    # ================================================================

    def get_hotkey(self):
        return self.hotkey_input.text().strip()

    def update_hotkey(self, new_hotkey):
        self.hotkey_input.setText(new_hotkey)

    def _refresh_latest_log_label(self):
        refresh_latest_log_label(self)

    def _apply_dialog_stylesheet(self):
        """根据当前主题生成并应用对话框样式表"""
        self.setStyleSheet(f"""
            #SettingsDialog {{
                background-color: {theme_surface_color()};
            }}
            QWidget#SettingsLeftPanel {{
                background-color: {theme_sidebar_color()};
                border-right: 1px solid {theme_border_color()};
            }}
            QWidget#SettingsRightArea {{
                background: transparent;
            }}
            QStackedWidget {{
                background: transparent;
            }}
        """)

    @safe_event
    def resizeEvent(self, e):
        super().resizeEvent(e)
        if hasattr(self, 'titleBar') and self.titleBar:
            self.titleBar.resize(self.width(), self.titleBar.height())

    @safe_event
    def showEvent(self, event):
        self._skip_unsaved_close_prompt = False
        self._apply_dialog_stylesheet()
        self.refresh_settings()
        self._settings_snapshot = self._snapshot_settings()
        super().showEvent(event)
        self._apply_taskbar_icon()

    # ================================================================
    # 未保存变更检测
    # ================================================================

    def _snapshot_settings(self):
        """捕获所有可编辑控件的当前值，返回 dict"""
        snap = {}
        # 文本类
        for attr in ('hotkey_input', 'hotkey_input_2', 'clipboard_hotkey_edit',
                      'clipboard_hotkey_edit_2', 'save_path_lbl', 'path_lbl',
                      'deepl_api_key_input'):
            w = getattr(self, attr, None)
            if w is not None:
                snap[attr] = w.text()
        # 开关类
        for attr in ('smart_toggle', 'save_toggle', 'ocr_enable_toggle',
                      'ocr_grayscale_toggle', 'ocr_upscale_toggle',
                      'deepl_pro_toggle', 'split_sentences_toggle',
                      'preserve_formatting_toggle', 'log_toggle',
                      'clipboard_enabled_toggle', 'clipboard_auto_paste_toggle',
                      'autostart_toggle', 'show_main_window_toggle',
                      'pin_auto_toolbar_toggle', 'info_hide_on_drag_toggle',
                      'preload_fonts_toggle', 'preload_screenshot_toggle',
                      'preload_toolbar_toggle', 'preload_ocr_toggle',
                      'preload_settings_toggle', 'preload_clipboard_toggle'):
            w = getattr(self, attr, None)
            if w is not None:
                snap[attr] = w.isChecked()
        # 下拉框类
        for attr in ('screenshot_format_combo', 'ocr_engine_combo',
                      'translation_target_combo', 'log_level_combo',
                      'language_combo', 'engine_combo', 'cursor_move_combo',
                      'magnifier_color_format_combo'):
            w = getattr(self, attr, None)
            if w is not None:
                snap[attr] = w.currentIndex()
        # 数值类
        for attr in ('log_retention_spinbox', 'clipboard_history_limit_spin',
                      'cooldown_spinbox', 'ocr_scale_spinbox'):
            w = getattr(self, attr, None)
            if w is not None:
                snap[attr] = w.value()
        # 应用内快捷键
        if hasattr(self, '_inapp_edits'):
            for cfg_key, edit in self._inapp_edits.items():
                snap[f'inapp_{cfg_key}'] = edit.text()
        # 颜色
        if hasattr(self, '_appearance_theme_color'):
            snap['theme_color'] = self._appearance_theme_color.name()
        if hasattr(self, '_appearance_mask_color'):
            snap['mask_color'] = self._appearance_mask_color.name()
        return snap

    def _has_unsaved_changes(self):
        """比较当前状态和快照，判断是否有未保存的变更"""
        if not hasattr(self, '_settings_snapshot'):
            return False
        current = self._snapshot_settings()
        return current != self._settings_snapshot

    def _confirm_close_with_unsaved_changes(self) -> str:
        """显示未保存变更确认框，返回 save/discard/cancel"""
        from ui.dialogs import show_custom_confirm_dialog
        
        buttons_config = [
            {"id": "save", "text": self.tr("Save"), "role": QDialogButtonBox.ButtonRole.AcceptRole},
            {"id": "discard", "text": self.tr("Don't Save"), "role": QDialogButtonBox.ButtonRole.DestructiveRole},
            {"id": "cancel", "text": self.tr("Cancel"), "role": QDialogButtonBox.ButtonRole.RejectRole, "default": True},
        ]
        
        return show_custom_confirm_dialog(
            self,
            self.tr("Unsaved Changes"),
            self.tr("You have unsaved changes. Do you want to save before closing?"),
            buttons_config
        )

    @safe_event
    def closeEvent(self, event):
        """关闭窗口前检查未保存变更"""
        if self._skip_unsaved_close_prompt:
            event.accept()
            return

        if self._has_unsaved_changes():
            action = self._confirm_close_with_unsaved_changes()
            if action == "save":
                self.accept()
                event.accept()
            elif action == "discard":
                event.accept()
            else:
                event.ignore()
        else:
            event.accept()

    def _apply_taskbar_icon(self):
        try:
            import ctypes, tempfile
            from PySide6.QtGui import QPixmap, QIcon, QPainter
            from core.resource_manager import ResourceManager

            _icon_path = ResourceManager.get_resource_path("svg/托盘.svg")
            if not os.path.exists(_icon_path):
                return

            pix = QPixmap(32, 32)
            pix.fill(Qt.GlobalColor.transparent)
            p = QPainter(pix)
            QIcon(_icon_path).paint(p, 0, 0, 32, 32)
            p.end()

            tmp_ico = os.path.join(tempfile.gettempdir(), "jietuba_win_icon.ico")
            pix.save(tmp_ico, "ICO")

            IMAGE_ICON = 1
            LR_LOADFROMFILE = 0x10
            hicon = ctypes.windll.user32.LoadImageW(None, tmp_ico, IMAGE_ICON, 32, 32, LR_LOADFROMFILE)
            if hicon:
                hwnd = int(self.winId())
                WM_SETICON = 0x0080
                ctypes.windll.user32.SendMessageW(hwnd, WM_SETICON, 1, hicon)
                ctypes.windll.user32.SendMessageW(hwnd, WM_SETICON, 0, hicon)
        except Exception as e:
            log_exception(e, "设置任务栏图标")

    def refresh_settings(self):
        """从配置管理器重新读取所有设置并更新界面"""
        if hasattr(self, 'hotkey_input'):
            self.hotkey_input.setText(self.config_manager.get_hotkey())
        if hasattr(self, 'hotkey_input_2'):
            self.hotkey_input_2.setText(self.config_manager.get_hotkey_2())
        if hasattr(self, 'clipboard_hotkey_edit'):
            self.clipboard_hotkey_edit.setText(self.config_manager.get_clipboard_hotkey())
        if hasattr(self, 'clipboard_hotkey_edit_2'):
            self.clipboard_hotkey_edit_2.setText(self.config_manager.get_clipboard_hotkey_2())

        # 应用内快捷键
        if hasattr(self, '_inapp_edits'):
            from .page_hotkey import INAPP_KEYS
            defaults_map = {k: d for k, _, d in INAPP_KEYS}
            for cfg_key, edit in self._inapp_edits.items():
                val = self.config_manager.get_inapp_shortcut(cfg_key)
                edit.setText(val or defaults_map.get(cfg_key, ""))
        if hasattr(self, 'cursor_move_combo'):
            mode = self.config_manager.get_inapp_cursor_move_mode()
            idx = self.cursor_move_combo.findData(mode)
            if idx >= 0:
                self.cursor_move_combo.setCurrentIndex(idx)

        if hasattr(self, 'engine_combo'):
            engine = self.config_manager.get_long_stitch_engine()
            index = self.engine_combo.findData(engine)
            if index >= 0:
                self.engine_combo.setCurrentIndex(index)
        if hasattr(self, 'cooldown_spinbox'):
            self.cooldown_spinbox.setValue(self.config_manager.settings.value('screenshot/scroll_cooldown', 0.15, type=float))

        if hasattr(self, 'smart_toggle'):
            self.smart_toggle.setChecked(self.config_manager.get_smart_selection())

        if hasattr(self, 'save_toggle'):
            self.save_toggle.setChecked(self.config_manager.get_screenshot_save_enabled())
        if hasattr(self, 'save_path_lbl'):
            self.save_path_lbl.setText(self.config_manager.get_screenshot_save_path())
        if hasattr(self, 'screenshot_format_combo'):
            idx = {"PNG": 0, "JPG": 1, "BMP": 2, "WEBP": 3}.get(
                self.config_manager.get_screenshot_format().upper(), 0)
            self.screenshot_format_combo.setCurrentIndex(idx)
        if hasattr(self, 'ocr_enable_toggle'):
            self.ocr_enable_toggle.setChecked(self.config_manager.get_ocr_enabled())
        if hasattr(self, 'ocr_engine_combo'):
            index = self.ocr_engine_combo.findData(self.config_manager.get_ocr_engine())
            if index >= 0:
                self.ocr_engine_combo.setCurrentIndex(index)
        if hasattr(self, 'ocr_grayscale_toggle'):
            self.ocr_grayscale_toggle.setChecked(self.config_manager.get_ocr_grayscale_enabled())
        if hasattr(self, 'ocr_upscale_toggle'):
            self.ocr_upscale_toggle.setChecked(self.config_manager.get_ocr_upscale_enabled())
        if hasattr(self, 'ocr_scale_spinbox'):
            self.ocr_scale_spinbox.setValue(self.config_manager.get_ocr_upscale_factor())

        if hasattr(self, 'deepl_api_key_input'):
            self.deepl_api_key_input.setText(self.config_manager.get_deepl_api_key())
        if hasattr(self, 'deepl_pro_toggle'):
            self.deepl_pro_toggle.setChecked(self.config_manager.get_deepl_use_pro())
        if hasattr(self, 'translation_target_combo'):
            index = self.translation_target_combo.findData(self.config_manager.get_app_setting("translation_target_lang", ""))
            if index >= 0:
                self.translation_target_combo.setCurrentIndex(index)
        if hasattr(self, 'split_sentences_toggle'):
            self.split_sentences_toggle.setChecked(self.config_manager.get_translation_split_sentences())
        if hasattr(self, 'preserve_formatting_toggle'):
            self.preserve_formatting_toggle.setChecked(self.config_manager.get_translation_preserve_formatting())

        if hasattr(self, 'log_toggle'):
            self.log_toggle.setChecked(self.config_manager.get_log_enabled())
        if hasattr(self, 'log_level_combo'):
            self.log_level_combo.setCurrentText(self.config_manager.get_log_level())
        if hasattr(self, 'log_retention_spinbox'):
            self.log_retention_spinbox.setValue(self.config_manager.get_log_retention_days())
        if hasattr(self, 'path_lbl'):
            self.path_lbl.setText(self.config_manager.get_log_dir())

        if hasattr(self, 'clipboard_enabled_toggle'):
            self.clipboard_enabled_toggle.setChecked(self.config_manager.get_clipboard_enabled())
        if hasattr(self, 'clipboard_auto_paste_toggle'):
            self.clipboard_auto_paste_toggle.setChecked(self.config_manager.get_clipboard_auto_paste())
        if hasattr(self, 'clipboard_history_limit_spin'):
            self.clipboard_history_limit_spin.setValue(self.config_manager.get_clipboard_history_limit())

        if hasattr(self, 'autostart_toggle'):
            from ..welcome.page6_finish import FinishPage as _FP
            self.autostart_toggle.setChecked(_FP._get_autostart())
        if hasattr(self, 'show_main_window_toggle'):
            self.show_main_window_toggle.setChecked(self.config_manager.get_show_main_window())
        if hasattr(self, 'pin_auto_toolbar_toggle'):
            self.pin_auto_toolbar_toggle.setChecked(self.config_manager.get_pin_auto_toolbar())
        if hasattr(self, 'language_combo'):
            index = self.language_combo.findData(self.config_manager.get_app_setting("language", "ja"))
            if index >= 0:
                self.language_combo.setCurrentIndex(index)

        # 预加载开关重置（默认全部 True）
        if hasattr(self, 'preload_fonts_toggle'):
            self.preload_fonts_toggle.setChecked(self.config_manager.get_app_setting("preload_fonts", True))
        if hasattr(self, 'preload_screenshot_toggle'):
            self.preload_screenshot_toggle.setChecked(self.config_manager.get_app_setting("preload_screenshot", True))
        if hasattr(self, 'preload_toolbar_toggle'):
            self.preload_toolbar_toggle.setChecked(self.config_manager.get_app_setting("preload_toolbar", True))
        if hasattr(self, 'preload_ocr_toggle'):
            self.preload_ocr_toggle.setChecked(self.config_manager.get_app_setting("preload_ocr", True))
        if hasattr(self, 'preload_settings_toggle'):
            self.preload_settings_toggle.setChecked(self.config_manager.get_app_setting("preload_settings", True))
        if hasattr(self, 'preload_clipboard_toggle'):
            self.preload_clipboard_toggle.setChecked(self.config_manager.get_app_setting("preload_clipboard", True))

        # 截图信息面板行为
        if hasattr(self, 'info_hide_on_drag_toggle'):
            self.info_hide_on_drag_toggle.setChecked(self.config_manager.get_app_setting("screenshot_info_hide_on_drag", False))

        # 外观设置
        if hasattr(self, '_theme_color_btn'):
            from core.theme import get_theme
            from .page_appearance import _update_color_btn
            theme = get_theme()
            self._appearance_theme_color = QColor(theme.theme_color)
            mc = theme.mask_color
            self._appearance_mask_color = QColor(mc.red(), mc.green(), mc.blue())
            _update_color_btn(self._theme_color_btn, self._appearance_theme_color)
            _update_color_btn(self._mask_color_btn, self._appearance_mask_color)

        # 剪切板主题色同步（在别处改了主题色后打开设置，确保显示最新值）
        if hasattr(self, '_clip_theme_btn'):
            from settings import get_tool_settings_manager
            from .page_appearance import _THEME_COLORS
            current_name = get_tool_settings_manager().get_clipboard_theme()
            self._clip_theme_name = current_name
            for tname, accent, bg in _THEME_COLORS:
                if tname == current_name:
                    self._clip_theme_btn.setStyleSheet(f"""
                        QPushButton {{
                            background: qlineargradient(x1:0,y1:0,x2:1,y2:0,
                                stop:0 {bg}, stop:0.5 {bg},
                                stop:0.5 {accent}, stop:1 {accent});
                            border: 2px solid {accent};
                            border-radius: 3px;
                        }}
                        QPushButton:hover {{ border: 2px solid #333; }}
                    """)
                    break 
