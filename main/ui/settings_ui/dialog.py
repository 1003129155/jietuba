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
    QVBoxLayout, QHBoxLayout, QLabel, QStackedWidget, QWidget, QDialogButtonBox,
    QFileDialog,
)
from PySide6.QtCore import Qt, Signal
from ui.dialogs import show_info_dialog, show_warning_dialog
from PySide6.QtGui import QColor, QFont, QIcon

from ui.fluent_lite import (
    NavigationInterface, NavigationItemPosition,
    FluentIcon, BodyLabel,
    PushButton as FluentPushButton,
    PrimaryPushButton, TransparentPushButton,
    FrostedFramelessDialog,
)
from ui.fluent_lite import FluentTitleBar, scrollbar_qss
from ui.fluent_lite.theme import ACCENT, ACCENT_HOVER, ACCENT_PRESSED

from core import log_info, safe_event
from core.logger import log_exception, T
from core.constants import CSS_FONT_FAMILY, DEFAULT_FONT_FAMILY
from core.ui_scale import configure_dialog_control, configure_dialog_controls, dialog_scaled
from settings.tool_settings import SMART_SELECTION_MODES

# 页面创建函数
from .page_hotkey import create_hotkey_page, validate_global_hotkey_edits
from .page_capture import create_capture_page
from .page_clipboard import create_clipboard_page
from .page_translation import create_translation_page
from . import provider_fields
from .page_log import create_log_page, refresh_latest_log_label
from .page_misc import create_misc_page
from .page_appearance import create_appearance_page
from .page_developer import create_developer_page
from .page_about import create_about_page
from .components import (
    theme_surface_color, theme_sidebar_color,
    theme_input_background, theme_popup_background,
    theme_popup_hover_background, theme_text_style, theme_menu_style, theme_color, refresh_theme_widget_styles,
    apply_theme_text_style,
)


class _FooterPrimaryButton(PrimaryPushButton):
    """Large dialog action button with a subtle trailing sparkle."""

    def __init__(self, text, parent=None):
        super().__init__(text, parent)
        self._sparkle = QLabel(self)
        self._sparkle.setFixedSize(dialog_scaled(18), dialog_scaled(22))
        self._sparkle.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self._sparkle.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
        self._sparkle.setPixmap(FluentIcon.SPARKLE.icon().pixmap(dialog_scaled(16), dialog_scaled(16)))
        self._sparkle.setStyleSheet("background: transparent; border: none;")

    def resizeEvent(self, event):
        super().resizeEvent(event)
        self._sparkle.move(self.width() - dialog_scaled(24), (self.height() - self._sparkle.height()) // 2)


def save_inapp_shortcut_edits(config_manager, edits):
    """Persist in-app editors while preserving empty-as-unbound semantics."""
    from core.shortcut_manager import is_reserved_inapp_shortcut

    for cfg_key, edit in edits.items():
        value = edit.text().strip()
        if value.endswith("+"):
            continue
        config_manager.set_inapp_shortcut(
            cfg_key, "" if is_reserved_inapp_shortcut(value) else value
        )


class SettingsDialog(FrostedFramelessDialog):
    """现代化设置对话框 - Fluent 风格（无系统标题栏）"""

    wizard_requested = Signal()

    def __init__(self, config_manager=None, current_hotkey="ctrl+shift+a", parent=None):
        super().__init__(parent)
        self.config_manager = config_manager
        # MainApp uses this after accepted() to decide whether the cached
        # settings window must be rebuilt.  Most settings can be refreshed in
        # place, but standalone-window sizing is calculated while widgets are
        # constructed, so reusing this instance would keep the old geometry.
        self._dialog_scale_changed_on_accept = False
        self.current_hotkey = current_hotkey
        self.main_window = parent
        self._skip_unsaved_close_prompt = False
        if self.config_manager is None:
            from .mock_config import MockConfig
            self.config_manager = MockConfig()

        # 自定义 Fluent 标题栏（在 setWindowTitle 之前，以接收信号）
        self._setup_titlebar()

        self.setWindowTitle("jietuba")
        self.resize(dialog_scaled(1050), dialog_scaled(750))
        self.setFont(QFont(DEFAULT_FONT_FAMILY, dialog_scaled(11)))
        self.setObjectName("SettingsDialog")

        self._setup_ui()
        from core.ui_theme import get_ui_theme
        get_ui_theme().theme_changed.connect(self._on_ui_theme_changed)

    def _setup_titlebar(self):
        """用 FluentTitleBar 替换默认标题栏"""
        title_bar = FluentTitleBar(self)
        configure_dialog_control(title_bar)
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
            log_exception(e, T("设置窗口图标"))

    # ================================================================
    # UI 构建
    # ================================================================

    def _setup_ui(self):
        sidebar_width = dialog_scaled(212)
        title_bar_height = self.titleBar.height() if getattr(self, 'titleBar', None) else dialog_scaled(32)

        main_layout = QHBoxLayout(self)
        main_layout.setContentsMargins(
            dialog_scaled(10), title_bar_height + dialog_scaled(6), dialog_scaled(10), dialog_scaled(10)
        )
        main_layout.setSpacing(dialog_scaled(10))

        # 1. 左侧面板
        left_panel = QWidget()
        left_panel.setObjectName("SettingsLeftPanel")
        left_panel.setFixedWidth(sidebar_width)
        left_v = QVBoxLayout(left_panel)
        left_v.setContentsMargins(dialog_scaled(10), dialog_scaled(6), dialog_scaled(10), dialog_scaled(10))
        left_v.setSpacing(dialog_scaled(8))

        # logo 区
        logo_area = QWidget()
        logo_area.setFixedHeight(dialog_scaled(78))
        logo_layout = QHBoxLayout(logo_area)
        logo_layout.setContentsMargins(
            dialog_scaled(12), dialog_scaled(10), dialog_scaled(12), dialog_scaled(10)
        )
        logo_layout.setSpacing(dialog_scaled(10))
        logo_layout.setAlignment(Qt.AlignmentFlag.AlignVCenter)

        logo_icon_lbl = QLabel()
        logo_icon_lbl.setFixedSize(dialog_scaled(36), dialog_scaled(36))
        logo_icon_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)

        try:
            from core.resource_manager import ResourceManager
            icon_path = ResourceManager.get_resource_path("svg/托盘.svg")
            if os.path.exists(icon_path):
                pm = QIcon(icon_path).pixmap(dialog_scaled(36), dialog_scaled(36))
                logo_icon_lbl.setPixmap(pm)
        except Exception as e:
            log_exception(e, T("加载 Logo 图标"))
        logo_layout.addWidget(logo_icon_lbl, 0, Qt.AlignmentFlag.AlignVCenter)

        text_box = QVBoxLayout()
        text_box.setContentsMargins(0, 0, 0, 0)
        text_box.setSpacing(dialog_scaled(2))
        app_name_lbl = BodyLabel(self.tr("jietuba"))
        apply_theme_text_style(app_name_lbl, 16, bold=True)
        app_desc_lbl = QLabel(self.tr("Settings"))
        apply_theme_text_style(app_desc_lbl, 13, caption=True)
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
        self.right_area = right_area
        right_area.setObjectName("SettingsRightArea")
        right_layout = QVBoxLayout(right_area)
        right_layout.setContentsMargins(
            dialog_scaled(24), dialog_scaled(16), dialog_scaled(24), dialog_scaled(18)
        )
        right_layout.setSpacing(dialog_scaled(14))

        self.content_title = QLabel(self.tr("Shortcut Settings"))
        apply_theme_text_style(
            self.content_title, 21, bold=True,
            extra=(
                f"padding: 0 {dialog_scaled(6)}px {dialog_scaled(2)}px {dialog_scaled(6)}px;"
            ),
        )

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

        # 九个分页都是一次性建完、切换只换可见性（不是懒加载/动态重建），
        # 建完后统一扫一遍即可覆盖全部分页里的 fluent_lite 控件。
        configure_dialog_controls(self.content_stack)

        right_layout.addWidget(self.content_title)
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
        nav.setExpandWidth(dialog_scaled(188))
        nav.setMinimumExpandWidth(0)
        nav.expand(useAni=False)
        nav.setMinimumWidth(dialog_scaled(188))
        nav.setMaximumWidth(dialog_scaled(196))

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
        apply_theme_text_style(lbl_title, 14)
        text_layout.addWidget(lbl_title)
        if desc:
            lbl_desc = QLabel(desc)
            apply_theme_text_style(lbl_desc, 13, caption=True)
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
        s = dialog_scaled
        return f"""
            QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox {{
                border: 1px solid {border_color}; border-radius: {s(4)}px;
                padding: {s(4)}px {s(8)}px; background-color: {input_bg};
                color: {text_color}; font-family: {CSS_FONT_FAMILY};
                font-size: {s(13)}px;
            }}
            QLineEdit:focus, QSpinBox:focus {{
                border: 1px solid {ACCENT}; background-color: {focus_bg};
            }}
            QSpinBox, QDoubleSpinBox {{ padding-right: {s(24)}px; }}
            QSpinBox::up-button, QDoubleSpinBox::up-button {{
                subcontrol-origin: border; subcontrol-position: top right;
                width: {s(20)}px; border-left: 1px solid {border_color};
                border-bottom: 1px solid {border_color}; border-top-right-radius: {s(4)}px;
                background: {input_bg};
            }}
            QSpinBox::up-button:hover, QDoubleSpinBox::up-button:hover {{ background: {popup_hover}; }}
            QSpinBox::up-button:pressed, QDoubleSpinBox::up-button:pressed {{ background: #DCE8F4; }}
            QSpinBox::up-arrow, QDoubleSpinBox::up-arrow {{
                image: none; border-left: {s(4)}px solid transparent;
                border-right: {s(4)}px solid transparent; border-bottom: {s(6)}px solid {arrow_color};
                width: 0; height: 0;
            }}
            QSpinBox::down-button, QDoubleSpinBox::down-button {{
                subcontrol-origin: border; subcontrol-position: bottom right;
                width: {s(20)}px; border-left: 1px solid {border_color};
                border-bottom-right-radius: {s(4)}px; background: {input_bg};
            }}
            QSpinBox::down-button:hover, QDoubleSpinBox::down-button:hover {{ background: {popup_hover}; }}
            QSpinBox::down-button:pressed, QDoubleSpinBox::down-button:pressed {{ background: #DCE8F4; }}
            QSpinBox::down-arrow, QDoubleSpinBox::down-arrow {{
                image: none; border-left: {s(4)}px solid transparent;
                border-right: {s(4)}px solid transparent; border-top: {s(6)}px solid {arrow_color};
                width: 0; height: 0;
            }}
            QComboBox::drop-down {{
                subcontrol-origin: padding; subcontrol-position: top right;
                width: {s(20)}px; border-left: 1px solid {border_color};
                border-top-right-radius: {s(4)}px; border-bottom-right-radius: {s(4)}px;
                background: {input_bg};
            }}
            QComboBox::down-arrow {{
                image: none; border-left: {s(4)}px solid transparent;
                border-right: {s(4)}px solid transparent; border-top: {s(6)}px solid {arrow_color};
                width: 0; height: 0; margin-right: {s(6)}px;
            }}
            QComboBox QAbstractItemView {{
                border: 1px solid {border_color}; background: {popup_bg};
                selection-background-color: {ACCENT}; selection-color: white;
                font-family: {CSS_FONT_FAMILY};
                font-size: {s(13)}px; color: {text_color}; outline: none;
            }}
            QComboBox QAbstractItemView::item {{
                padding: {s(6)}px {s(8)}px; min-height: {s(24)}px; color: {text_color}; background: {popup_bg};
            }}
            QComboBox QAbstractItemView::item:hover {{
                background-color: {popup_hover}; color: {text_color};
            }}
            QComboBox QAbstractItemView::item:selected {{
                background-color: {ACCENT}; color: white;
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
            self._refresh_after_page_change()

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
        self.nav_list.clearCurrentItem()
        self._refresh_after_page_change()

    def _refresh_after_page_change(self):
        """Clear the translucent backing store before painting a new page."""
        self.update()
        self.right_area.update()
        self.content_stack.update()
        current = self.content_stack.currentWidget()
        if current is not None:
            current.update()

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
        s = dialog_scaled
        layout = QHBoxLayout()
        layout.setSpacing(s(12))

        # The footer sits outside content_stack, so its controls are not
        # covered by the sweep in _setup_ui and have to opt in themselves.
        reset_btn = TransparentPushButton(self.tr("Reset This Page"))
        configure_dialog_control(reset_btn)
        reset_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        reset_btn.setIcon(FluentIcon.DELETE)
        reset_btn.clicked.connect(self._reset_current_page)

        cancel_btn = FluentPushButton(self.tr("Cancel"))
        configure_dialog_control(cancel_btn)
        self._footer_cancel_btn = cancel_btn
        cancel_btn.setFixedHeight(s(46))
        cancel_btn.setMinimumWidth(s(150))
        cancel_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        cancel_btn.setIcon(FluentIcon.CANCEL)
        cancel_btn.setBaseIconSize(22)
        cancel_btn.setStyleSheet(f"""
            QPushButton {{
                min-height: {s(44)}px;
                padding: 0 {s(24)}px;
                color: #20262D;
                background: rgba(255, 255, 255, 0.18);
                border: 1px solid rgba(77, 88, 101, 0.38);
                border-radius: {s(12)}px;
                font-size: {s(17)}px;
                font-weight: 500;
                outline: none;
            }}
            QPushButton:hover {{
                background: rgba(255, 255, 255, 0.46);
                border-color: rgba(55, 68, 82, 0.55);
            }}
            QPushButton:pressed {{
                background: rgba(220, 228, 236, 0.60);
                border-color: rgba(55, 68, 82, 0.66);
            }}
        """)
        cancel_btn.clicked.connect(self.reject)

        ok_btn = _FooterPrimaryButton(self.tr("Apply"))
        configure_dialog_control(ok_btn)
        self._footer_ok_btn = ok_btn
        ok_btn.setFixedHeight(s(46))
        ok_btn.setMinimumWidth(s(150))
        ok_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        ok_btn.setIcon(FluentIcon.CHECK)
        ok_btn.setBaseIconSize(23)
        ok_btn.setStyleSheet("""
            QPushButton {
                min-height: %dpx;
                padding: 0 %dpx;
                color: white;
                background: qlineargradient(x1:0, y1:0, x2:1, y2:1,
                    stop:0 #91AABD, stop:0.52 %s, stop:1 #607F9A);
                border: 1px solid rgba(255, 255, 255, 0.34);
                border-radius: %dpx;
                font-size: %dpx;
                font-weight: 600;
                outline: none;
            }
            QPushButton:hover {
                background: qlineargradient(x1:0, y1:0, x2:1, y2:1,
                    stop:0 #9AAFC0, stop:0.52 %s, stop:1 #58748D);
            }
            QPushButton:pressed {
                background: qlineargradient(x1:0, y1:0, x2:1, y2:1,
                    stop:0 %s, stop:1 #465E73);
                padding-top: 2px;
            }
            QPushButton:disabled {
                color: rgba(255, 255, 255, 0.72);
                background: rgba(151, 170, 186, 0.68);
            }
        """ % (s(44), s(25), ACCENT, s(12), s(17), ACCENT_HOVER, ACCENT_PRESSED))
        ok_btn.clicked.connect(self.accept)
        self._apply_footer_styles()

        layout.addWidget(reset_btn)
        layout.addStretch()
        layout.addWidget(cancel_btn)
        layout.addWidget(ok_btn)
        return layout

    def _apply_footer_styles(self):
        from core.ui_theme import get_ui_theme
        t = get_ui_theme().tokens
        s = dialog_scaled
        cancel_btn = getattr(self, "_footer_cancel_btn", None)
        if cancel_btn is not None:
            cancel_btn.setStyleSheet(f"""
                QPushButton {{
                    min-height: {s(44)}px;
                    padding: 0 {s(24)}px;
                    color: {t.text};
                    background: {t.surface};
                    border: 1px solid {t.border_hover};
                    border-radius: {s(12)}px;
                    font-size: {s(17)}px;
                    font-weight: 500;
                    outline: none;
                }}
                QPushButton:hover {{
                    background: {t.surface_hover};
                    border-color: {t.border_hover};
                }}
                QPushButton:pressed {{
                    background: {t.surface_subtle};
                }}
            """)
        ok_btn = getattr(self, "_footer_ok_btn", None)
        if ok_btn is not None:
            ok_btn.setStyleSheet(f"""
                QPushButton {{
                    min-height: {s(44)}px;
                    padding: 0 {s(25)}px;
                    color: #FFFFFF;
                    background: {t.accent};
                    border: 1px solid rgba(255, 255, 255, 0.28);
                    border-radius: {s(12)}px;
                    font-size: {s(17)}px;
                    font-weight: 600;
                    outline: none;
                }}
                QPushButton:hover {{
                    background: {t.accent_hover};
                }}
                QPushButton:pressed {{
                    background: {t.accent_pressed};
                    padding-top: 2px;
                }}
                QPushButton:disabled {{
                    color: rgba(255, 255, 255, 0.72);
                    background: #687D8F;
                }}
            """)

    # ================================================================
    # 重置页面
    # ================================================================

    def _save_magnifier_color_formats(self):
        """颜色格式列表是在管理窗口里编辑的，先留在实例上，点「应用」才落盘。"""
        formats = getattr(self, 'magnifier_color_formats', None)
        if formats is None:
            return
        from settings import color_formats
        color_formats.save(self.config_manager, formats)

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
        if hasattr(self, 'pin_clipboard_hotkey_edit'):
            self.pin_clipboard_hotkey_edit.setText(defaults["pin_clipboard_hotkey"])
        if hasattr(self, 'pin_clipboard_hotkey_edit_2'):
            self.pin_clipboard_hotkey_edit_2.setText(defaults["pin_clipboard_hotkey_2"])
        if hasattr(self, 'translation_hotkey_edit'):
            self.translation_hotkey_edit.setText(defaults["translation_hotkey"])
        if hasattr(self, 'translation_hotkey_edit_2'):
            self.translation_hotkey_edit_2.setText(defaults["translation_hotkey_2"])
        # 应用内快捷键
        if hasattr(self, '_inapp_edits'):
            for cfg_key, edit in self._inapp_edits.items():
                edit.setText(defaults.get(cfg_key, ""))
        if hasattr(self, 'cursor_move_combo'):
            idx = self.cursor_move_combo.findData(defaults["inapp_cursor_move_mode"])
            if idx >= 0:
                self.cursor_move_combo.setCurrentIndex(idx)

    def _reset_long_screenshot_page(self):
        """重置开发者选项页。"""
        defaults = self.config_manager.APP_DEFAULT_SETTINGS
        if hasattr(self, 'engine_combo'):
            index = self.engine_combo.findData(defaults["long_stitch_engine"])
            if index >= 0:
                self.engine_combo.setCurrentIndex(index)
        if hasattr(self, 'cooldown_spinbox'):
            self.cooldown_spinbox.setValue(defaults["scroll_cooldown"])
        if hasattr(self, 'ignore_top_pixels_spinbox'):
            self.ignore_top_pixels_spinbox.setValue(defaults["long_stitch_ignore_top_pixels"])
        for key in (
            "preload_screenshot", "preload_toolbar", "preload_ocr",
            "preload_settings", "preload_clipboard",
        ):
            toggle = getattr(self, f"{key}_toggle", None)
            if toggle is not None:
                toggle.setChecked(defaults[key])
        if hasattr(self, 'info_hide_on_drag_toggle'):
            self.info_hide_on_drag_toggle.setChecked(
                defaults["screenshot_info_hide_on_drag"]
            )

    def _reset_appearance_page(self):
        """重置外观设置页面"""
        from .page_appearance import _update_color_btn
        defaults = self.config_manager.APP_DEFAULT_SETTINGS
        if hasattr(self, '_ui_theme_combo'):
            index = self._ui_theme_combo.findData(
                defaults.get("ui_theme_mode", "system")
            )
            if index >= 0:
                self._ui_theme_combo.setCurrentIndex(index)
        if hasattr(self, '_ui_scale_combo'):
            index = self._ui_scale_combo.findData(defaults.get("ui_scale_percent", 100))
            if index >= 0:
                self._ui_scale_combo.setCurrentIndex(index)
        if hasattr(self, '_dialog_scale_combo'):
            index = self._dialog_scale_combo.findData(defaults.get("dialog_scale_percent", 100))
            if index >= 0:
                self._dialog_scale_combo.setCurrentIndex(index)
        if hasattr(self, '_appearance_theme_color'):
            self._appearance_theme_color = QColor(defaults["theme_color"])
            _update_color_btn(self._theme_color_btn, self._appearance_theme_color)
        if hasattr(self, '_appearance_mask_color'):
            self._appearance_mask_color = QColor(
                defaults["mask_color_r"], defaults["mask_color_g"],
                defaults["mask_color_b"]
            )
            _update_color_btn(self._mask_color_btn, self._appearance_mask_color)
        if hasattr(self, '_selection_border_combo'):
            index = self._selection_border_combo.findData(defaults["selection_border_width"])
            if index >= 0:
                self._selection_border_combo.setCurrentIndex(index)
        if hasattr(self, '_selection_handle_combo'):
            index = self._selection_handle_combo.findData(defaults["selection_handle_style"])
            if index >= 0:
                self._selection_handle_combo.setCurrentIndex(index)
        if hasattr(self, '_selection_handle_size_combo'):
            index = self._selection_handle_size_combo.findData(
                defaults["selection_handle_size"]
            )
            if index >= 0:
                self._selection_handle_size_combo.setCurrentIndex(index)

    def _reset_screenshot_settings_page(self):
        defaults = self.config_manager.APP_DEFAULT_SETTINGS
        if hasattr(self, 'double_click_copy_close_toggle'):
            self.double_click_copy_close_toggle.setChecked(
                defaults["double_click_copy_close"]
            )
        if hasattr(self, 'cross_tool_selection_toggle'):
            self.cross_tool_selection_toggle.setChecked(
                defaults["cross_tool_selection"]
            )
        if hasattr(self, 'text_always_on_top_toggle'):
            self.text_always_on_top_toggle.setChecked(
                defaults["text_always_on_top"]
            )
        if hasattr(self, 'smart_mode_combo'):
            self.smart_mode_combo.setCurrentIndex(SMART_SELECTION_MODES.index(
                defaults["smart_selection_mode"] if defaults["smart_selection"] else "off"
            ))
        if hasattr(self, 'smart_animation_toggle'):
            self.smart_animation_toggle.setChecked(defaults["smart_selection_animation"])
        if hasattr(self, 'save_toggle'):
            self.save_toggle.setChecked(defaults["screenshot_save_enabled"])
        if hasattr(self, 'clipboard_file_reference_toggle'):
            self.clipboard_file_reference_toggle.setChecked(
                defaults["clipboard_file_reference_enabled"]
            )
        if hasattr(self, 'save_path_lbl'):
            self.save_path_lbl.setText(defaults["screenshot_save_path"])
        if hasattr(self, 'screenshot_format_combo'):
            idx = {"PNG": 0, "JPG": 1, "BMP": 2, "WEBP": 3, "PDF": 4}.get(defaults["screenshot_format"].upper(), 0)
            self.screenshot_format_combo.setCurrentIndex(idx)
        for attr, key in (('magnifier_enabled_toggle', 'magnifier_enabled'),
                          ('magnifier_grid_toggle', 'magnifier_grid'),
                          ('magnifier_swatch_toggle', 'magnifier_swatch'),
                          ('magnifier_hint_toggle', 'magnifier_hint')):
            toggle = getattr(self, attr, None)
            if toggle is not None:
                toggle.setChecked(defaults[key])
        if hasattr(self, 'magnifier_color_formats'):
            from settings import color_formats
            self.magnifier_color_formats = color_formats.default_formats()
        if hasattr(self, 'pin_auto_toolbar_toggle'):
            self.pin_auto_toolbar_toggle.setChecked(defaults["pin_auto_toolbar"])
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
        if hasattr(self, 'log_retention_combo'):
            index = self.log_retention_combo.findData(defaults["log_retention_days"])
            if index < 0:
                index = self.log_retention_combo.findData(7)
            if index >= 0:
                self.log_retention_combo.setCurrentIndex(index)
        if hasattr(self, 'path_lbl'):
            self.path_lbl.setText(defaults["log_dir"])

    def _reset_misc_page(self):
        defaults = self.config_manager.APP_DEFAULT_SETTINGS
        if hasattr(self, 'autostart_toggle'):
            self.autostart_toggle.setChecked(False)
        if hasattr(self, 'show_main_window_toggle'):
            self.show_main_window_toggle.setChecked(defaults["show_main_window"])

    def _reset_translation_page(self):
        defaults = self.config_manager.APP_DEFAULT_SETTINGS
        if hasattr(self, 'translation_provider_combo'):
            index = self.translation_provider_combo.findData(
                defaults["translation_provider"]
            )
            if index >= 0:
                self.translation_provider_combo.setCurrentIndex(index)
        # 各家凭据和独有开关都按注册表的声明恢复，不再一家家手写。
        # 手写那版加 baidu 时漏过一次，而漏了不报错——恢复默认会默默少恢复两项。
        if hasattr(self, 'provider_field_widgets'):
            provider_fields.reset_to_defaults(
                defaults,
                provider_fields.all_fields(self.translation_registry),
                self.provider_field_widgets,
            )
        if hasattr(self, 'translation_target_combo'):
            index = self.translation_target_combo.findData(
                defaults["translation_target_lang"]
            )
            if index >= 0:
                self.translation_target_combo.setCurrentIndex(index)
        if hasattr(self, 'split_sentences_toggle'):
            self.split_sentences_toggle.setChecked(
                defaults["translation_split_sentences"]
            )
        if hasattr(self, 'preserve_formatting_toggle'):
            self.preserve_formatting_toggle.setChecked(
                defaults["translation_preserve_formatting"]
            )

    def _reset_clipboard_page(self):
        defaults = self.config_manager.APP_DEFAULT_SETTINGS
        if hasattr(self, 'clipboard_enabled_toggle'):
            self.clipboard_enabled_toggle.setChecked(defaults["clipboard_enabled"])
        if hasattr(self, 'clipboard_auto_paste_toggle'):
            self.clipboard_auto_paste_toggle.setChecked(defaults["clipboard_auto_paste"])
        if hasattr(self, 'clipboard_history_limit_spin'):
            self.clipboard_history_limit_spin.setValue(defaults["clipboard_history_limit"])
        if hasattr(self, 'clipboard_scan_interval_combo'):
            idx = self.clipboard_scan_interval_combo.findData(
                defaults["clipboard_foreground_scan_interval_ms"]
            )
            if idx >= 0:
                self.clipboard_scan_interval_combo.setCurrentIndex(idx)

    # ================================================================
    # 保存（accept）
    # ================================================================

    def accept(self):
        """保存所有设置"""
        self._dialog_scale_changed_on_accept = False

        # 六个全局快捷键必须先整体通过校验。这里发生在任何 set_* 之前，
        # 因而冲突值不会写入配置，窗口也不会关闭。
        if not validate_global_hotkey_edits(self, check_system=True):
            self.content_stack.setCurrentIndex(0)
            self._set_current_nav("shortcuts")
            show_warning_dialog(
                self,
                self.tr("Shortcut Conflict"),
                self.tr(
                    "Some global hotkeys are duplicated or unavailable. "
                    "Please fix them before applying."
                ),
            )
            return

        # 防止保存过程中（比如语言切换触发的窗口重建）触发未保存确认弹窗
        self._skip_unsaved_close_prompt = True

        # 0. 快捷键
        self.config_manager.set_hotkey(self.hotkey_input.text().strip())
        if hasattr(self, 'hotkey_input_2'):
            self.config_manager.set_hotkey_2(self.hotkey_input_2.text().strip())
        if hasattr(self, 'translation_hotkey_edit'):
            self.config_manager.set_translation_hotkey(
                self.translation_hotkey_edit.text().strip()
            )
        if hasattr(self, 'translation_hotkey_edit_2'):
            self.config_manager.set_translation_hotkey_2(
                self.translation_hotkey_edit_2.text().strip()
            )

        # 1. 截图交互（双击确认 + 智能选区）
        if hasattr(self, 'double_click_copy_close_toggle'):
            self.config_manager.set_double_click_copy_close_enabled(
                self.double_click_copy_close_toggle.isChecked()
            )
        if hasattr(self, 'cross_tool_selection_toggle'):
            self.config_manager.set_cross_tool_selection_enabled(
                self.cross_tool_selection_toggle.isChecked()
            )
        if hasattr(self, 'text_always_on_top_toggle'):
            self.config_manager.set_text_always_on_top_enabled(
                self.text_always_on_top_toggle.isChecked()
            )
        if hasattr(self, 'smart_mode_combo'):
            self.config_manager.set_smart_selection_mode(
                self.smart_mode_combo.currentData()
            )
        if hasattr(self, 'smart_animation_toggle'):
            self.config_manager.set_smart_selection_animation(
                self.smart_animation_toggle.isChecked()
            )

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

            if hasattr(self, 'log_retention_combo'):
                retention_days = self.log_retention_combo.currentData()
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
        if hasattr(self, 'clipboard_file_reference_toggle'):
            self.config_manager.set_clipboard_file_reference_enabled(
                self.clipboard_file_reference_toggle.isChecked()
            )
        if hasattr(self, 'save_path_lbl'):
            self.config_manager.set_screenshot_save_path(self.save_path_lbl.text())
        if hasattr(self, 'screenshot_format_combo'):
            self.config_manager.set_screenshot_format(self.screenshot_format_combo.currentData())

        # 3.5 放大镜
        for attr, key in (('magnifier_enabled_toggle', 'magnifier_enabled'),
                          ('magnifier_grid_toggle', 'magnifier_grid'),
                          ('magnifier_swatch_toggle', 'magnifier_swatch'),
                          ('magnifier_hint_toggle', 'magnifier_hint')):
            toggle = getattr(self, attr, None)
            if toggle is not None:
                self.config_manager.set_app_setting(key, toggle.isChecked())

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
        if hasattr(self, 'translation_provider_combo'):
            self.config_manager.set_translation_provider(
                self.translation_provider_combo.currentData()
            )
        # 各家凭据和独有开关统一按注册表的声明保存。以前这里是一长串手写的
        # if hasattr(...)，加一家就得补一段；补漏了不报错——hasattr 把
        # AttributeError 一起吞了，表现成「填了、存不上、一直说未配置」。
        # baidu 就这么漏过一次，而当时全部测试都是绿的。
        if hasattr(self, 'provider_field_widgets'):
            provider_fields.save_from(
                self.config_manager,
                provider_fields.all_fields(self.translation_registry),
                self.provider_field_widgets,
            )
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
        self._save_magnifier_color_formats()

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
        if hasattr(self, 'clipboard_scan_interval_combo'):
            interval_ms = self.clipboard_scan_interval_combo.currentData()
            if interval_ms is not None:
                self.config_manager.set_clipboard_foreground_scan_interval_ms(interval_ms)
        if hasattr(self, 'clipboard_hotkey_edit'):
            self.config_manager.set_clipboard_hotkey(self.clipboard_hotkey_edit.text().strip())
        if hasattr(self, 'clipboard_hotkey_edit_2'):
            self.config_manager.set_clipboard_hotkey_2(self.clipboard_hotkey_edit_2.text().strip())
        if hasattr(self, 'pin_clipboard_hotkey_edit'):
            self.config_manager.set_pin_clipboard_hotkey(self.pin_clipboard_hotkey_edit.text().strip())
        if hasattr(self, 'pin_clipboard_hotkey_edit_2'):
            self.config_manager.set_pin_clipboard_hotkey_2(self.pin_clipboard_hotkey_edit_2.text().strip())

        # 7.5 应用内快捷键
        if hasattr(self, '_inapp_edits'):
            save_inapp_shortcut_edits(self.config_manager, self._inapp_edits)
            # 通知钉图快捷键 handler 重新加载绑定
            try:
                from pin.pin_shortcut import PinShortcutController
                pin_ctrl = PinShortcutController._instance
                if pin_ctrl is not None:
                    pin_ctrl._edit_handler.reload_bindings()
                    pin_ctrl._normal_handler.reload_bindings()
            except Exception as e:
                log_exception(e, T("重载 Pin 快捷键绑定"))
        if hasattr(self, 'cursor_move_combo'):
            self.config_manager.set_inapp_cursor_move_mode(
                self.cursor_move_combo.currentData()
            )

        # 8. 长截图/开发者
        if hasattr(self, 'engine_combo'):
            self.config_manager.set_long_stitch_engine(self.engine_combo.currentData())
        if hasattr(self, 'cooldown_spinbox'):
            self.config_manager.set_scroll_cooldown(self.cooldown_spinbox.value())
        if hasattr(self, 'ignore_top_pixels_spinbox'):
            self.config_manager.set_long_stitch_ignore_top_pixels(self.ignore_top_pixels_spinbox.value())

        # 9. 预加载开关（重启生效）
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

        # 11. 外观设置（主题色、遮罩色、选区外观、界面缩放）
        if hasattr(self, '_ui_theme_combo'):
            from core.ui_theme import get_ui_theme
            get_ui_theme().set_mode(self._ui_theme_combo.currentData())

        # 缩放比例一变，已建出来的工具栏/面板会自己收到 scale_changed 重算
        if hasattr(self, '_ui_scale_combo'):
            from core.ui_scale import get_ui_scale
            get_ui_scale().set_percent(self._ui_scale_combo.currentData())

        # 窗口缩放在后续新建的独立窗口中按各自基准尺寸生效；这里不对当前设置
        # 窗口做事后整体拉伸，避免固定高度控件与已布局内容相互挤压。
        if hasattr(self, '_dialog_scale_combo'):
            from core.ui_scale import get_dialog_scale
            self._dialog_scale_changed_on_accept = get_dialog_scale().set_percent(
                self._dialog_scale_combo.currentData()
            )

        from core.theme import get_theme
        theme = get_theme()
        if hasattr(self, '_appearance_theme_color'):
            theme.set_theme_color(self._appearance_theme_color)
        if hasattr(self, '_appearance_mask_color'):
            theme.set_mask_color(self._appearance_mask_color)
        if hasattr(self, '_selection_border_combo'):
            theme.set_selection_border_width(self._selection_border_combo.currentData())
        if hasattr(self, '_selection_handle_combo'):
            theme.set_selection_handle_style(self._selection_handle_combo.currentData())
        if hasattr(self, '_selection_handle_size_combo'):
            theme.set_selection_handle_size(self._selection_handle_size_combo.currentData())

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
        from core.ui_theme import get_ui_theme
        tokens = get_ui_theme().tokens
        self.setStyleSheet(f"""
            #SettingsDialog {{
                background: transparent;
                border: none;
            }}
            QWidget#SettingsLeftPanel {{
                background-color: {theme_sidebar_color()};
                border: none;
                border-radius: 8px;
            }}
            QWidget#SettingsRightArea {{
                background: {theme_surface_color()};
                border: none;
                border-radius: 8px;
            }}
            QLabel {{
                color: {tokens.text};
            }}
            QStackedWidget {{
                background: transparent;
            }}
        """ + scrollbar_qss(self))

    def _on_ui_theme_changed(self, _tokens):
        """Rebuild window-local styles after an OS or user theme change."""
        self._apply_dialog_stylesheet()
        refresh_theme_widget_styles(self)
        self._apply_footer_styles()
        input_style = self._get_input_style()
        for attr in (
            "hotkey_input", "hotkey_input_2",
            "clipboard_hotkey_edit", "clipboard_hotkey_edit_2",
            "translation_hotkey_edit", "translation_hotkey_edit_2",
            "pin_clipboard_hotkey_edit", "pin_clipboard_hotkey_edit_2",
        ):
            widget = getattr(self, attr, None)
            if widget is not None:
                widget.setStyleSheet(input_style)
        # 翻译凭据的输入框是按声明动态建的，从 provider_field_widgets 取。
        # 上面那张手写清单里 azure/baidu 一直缺席，切主题时它们不跟着变。
        for widget in getattr(self, "provider_field_widgets", {}).values():
            if hasattr(widget, "setEchoMode"):
                widget.setStyleSheet(input_style)
        for widget in getattr(self, "_inapp_edits", {}).values():
            widget.setStyleSheet(input_style)
        self.content_title.setStyleSheet(
            theme_text_style(
                21, bold=True,
                extra="padding: 0 6px 2px 6px;"
            )
        )
        self.update()

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

    @safe_event
    def hideEvent(self, event):
        # 这里在隐藏时强制复位所有标题栏按钮状态。
        if getattr(self, 'titleBar', None):
            try:
                from qframelesswindow.titlebar.title_bar_buttons import (
                    TitleBarButton, TitleBarButtonState,
                )
                for btn in self.titleBar.findChildren(TitleBarButton):
                    btn.setState(TitleBarButtonState.NORMAL)
            except Exception as e:
                log_exception(e, T("重置标题栏按钮状态"))
        super().hideEvent(event)

    # ================================================================
    # 未保存变更检测
    # ================================================================

    def _snapshot_settings(self):
        """捕获所有可编辑控件的当前值，返回 dict"""
        snap = {}
        # 文本类
        for attr in ('hotkey_input', 'hotkey_input_2', 'clipboard_hotkey_edit',
                      'translation_hotkey_edit', 'translation_hotkey_edit_2',
                      'clipboard_hotkey_edit_2', 'pin_clipboard_hotkey_edit',
                      'pin_clipboard_hotkey_edit_2', 'save_path_lbl', 'path_lbl',
                      ):
            w = getattr(self, attr, None)
            if w is not None:
                snap[attr] = w.text()
        # 翻译凭据按声明取。以前是手写清单，只列了 deepl/amazon/google——改动
        # azure 或 baidu 的凭据后直接关窗，不会弹「未保存」提示，改动就没了。
        if hasattr(self, 'provider_field_widgets'):
            for f in provider_fields.all_fields(self.translation_registry):
                w = self.provider_field_widgets.get(f.config_key)
                if w is not None:
                    snap[f.config_key] = provider_fields.widget_value(f, w)
        # 开关类
        for attr in ('double_click_copy_close_toggle',
                      'cross_tool_selection_toggle',
                      'text_always_on_top_toggle',
                      'smart_animation_toggle',
                      'save_toggle', 'clipboard_file_reference_toggle',
                      'magnifier_enabled_toggle', 'magnifier_grid_toggle',
                      'magnifier_swatch_toggle', 'magnifier_hint_toggle',
                      'ocr_enable_toggle',
                      'ocr_grayscale_toggle', 'ocr_upscale_toggle',
                      'split_sentences_toggle',
                      'preserve_formatting_toggle', 'log_toggle',
                      'clipboard_enabled_toggle', 'clipboard_auto_paste_toggle',
                      'autostart_toggle', 'show_main_window_toggle',
                      'pin_auto_toolbar_toggle', 'info_hide_on_drag_toggle',
                      'preload_screenshot_toggle',
                      'preload_toolbar_toggle', 'preload_ocr_toggle',
                      'preload_settings_toggle', 'preload_clipboard_toggle'):
            w = getattr(self, attr, None)
            if w is not None:
                snap[attr] = w.isChecked()
        # 下拉框类
        for attr in ('screenshot_format_combo', 'ocr_engine_combo',
                      'translation_provider_combo', 'translation_target_combo',
                      'log_level_combo',
                      'language_combo', 'engine_combo', 'cursor_move_combo',
                      'log_retention_combo',
                      '_ui_theme_combo', '_ui_scale_combo', '_dialog_scale_combo',
                      '_selection_border_combo', '_selection_handle_combo',
                      '_selection_handle_size_combo', 'smart_mode_combo',
                      'clipboard_scan_interval_combo'):
            w = getattr(self, attr, None)
            if w is not None:
                snap[attr] = w.currentIndex()
        # 数值类
        for attr in ('clipboard_history_limit_spin',
                      'cooldown_spinbox', 'ignore_top_pixels_spinbox',
                      'ocr_scale_spinbox'):
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
            log_exception(e, T("设置任务栏图标"))

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
        if hasattr(self, 'pin_clipboard_hotkey_edit'):
            self.pin_clipboard_hotkey_edit.setText(self.config_manager.get_pin_clipboard_hotkey())
        if hasattr(self, 'pin_clipboard_hotkey_edit_2'):
            self.pin_clipboard_hotkey_edit_2.setText(self.config_manager.get_pin_clipboard_hotkey_2())
        if hasattr(self, 'translation_hotkey_edit'):
            self.translation_hotkey_edit.setText(
                self.config_manager.get_translation_hotkey()
            )
        if hasattr(self, 'translation_hotkey_edit_2'):
            self.translation_hotkey_edit_2.setText(
                self.config_manager.get_translation_hotkey_2()
            )

        # 应用内快捷键
        if hasattr(self, '_inapp_edits'):
            from core.shortcut_manager import is_reserved_inapp_shortcut
            for cfg_key, edit in self._inapp_edits.items():
                val = self.config_manager.get_inapp_shortcut(cfg_key)
                edit.setText("" if is_reserved_inapp_shortcut(val) else val)
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
            self.cooldown_spinbox.setValue(self.config_manager.get_scroll_cooldown())
        if hasattr(self, 'ignore_top_pixels_spinbox'):
            self.ignore_top_pixels_spinbox.setValue(self.config_manager.get_long_stitch_ignore_top_pixels())

        if hasattr(self, 'smart_mode_combo'):
            self.smart_mode_combo.setCurrentIndex(SMART_SELECTION_MODES.index(
                self.config_manager.get_smart_selection_mode()
            ))

        if hasattr(self, 'smart_animation_toggle'):
            self.smart_animation_toggle.setChecked(
                self.config_manager.get_smart_selection_animation()
            )

        if hasattr(self, 'double_click_copy_close_toggle'):
            self.double_click_copy_close_toggle.setChecked(
                self.config_manager.get_double_click_copy_close_enabled()
            )

        if hasattr(self, 'cross_tool_selection_toggle'):
            self.cross_tool_selection_toggle.setChecked(
                self.config_manager.get_cross_tool_selection_enabled()
            )

        if hasattr(self, 'text_always_on_top_toggle'):
            self.text_always_on_top_toggle.setChecked(
                self.config_manager.get_text_always_on_top_enabled()
            )

        if hasattr(self, 'save_toggle'):
            self.save_toggle.setChecked(self.config_manager.get_screenshot_save_enabled())
        if hasattr(self, 'clipboard_file_reference_toggle'):
            self.clipboard_file_reference_toggle.setChecked(
                self.config_manager.get_clipboard_file_reference_enabled()
            )
        if hasattr(self, 'save_path_lbl'):
            self.save_path_lbl.setText(self.config_manager.get_screenshot_save_path())
        if hasattr(self, 'screenshot_format_combo'):
            idx = {"PNG": 0, "JPG": 1, "BMP": 2, "WEBP": 3, "PDF": 4}.get(
                self.config_manager.get_screenshot_format().upper(), 0)
            self.screenshot_format_combo.setCurrentIndex(idx)
        for attr, key in (('magnifier_enabled_toggle', 'magnifier_enabled'),
                          ('magnifier_grid_toggle', 'magnifier_grid'),
                          ('magnifier_swatch_toggle', 'magnifier_swatch'),
                          ('magnifier_hint_toggle', 'magnifier_hint')):
            toggle = getattr(self, attr, None)
            if toggle is not None:
                toggle.setChecked(self.config_manager.get_app_setting(key))
        if hasattr(self, 'magnifier_color_formats'):
            from settings import color_formats
            self.magnifier_color_formats = color_formats.load(self.config_manager)
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

        if hasattr(self, 'translation_provider_combo'):
            index = self.translation_provider_combo.findData(
                self.config_manager.get_translation_provider()
            )
            if index >= 0:
                self.translation_provider_combo.setCurrentIndex(index)
        # 各家凭据/开关按声明重载。手写那版只覆盖了 deepl/amazon/google，
        # azure 和 baidu 从来没被重载过——外部改了配置再打开设置页，看到的
        # 还是旧值。
        if hasattr(self, 'provider_field_widgets'):
            provider_fields.load_into(
                self.config_manager,
                provider_fields.all_fields(self.translation_registry),
                self.provider_field_widgets,
            )
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
        if hasattr(self, 'log_retention_combo'):
            index = self.log_retention_combo.findData(self.config_manager.get_log_retention_days())
            if index < 0:
                index = self.log_retention_combo.findData(7)
            if index >= 0:
                self.log_retention_combo.setCurrentIndex(index)
        if hasattr(self, 'path_lbl'):
            self.path_lbl.setText(self.config_manager.get_log_dir())

        if hasattr(self, 'clipboard_enabled_toggle'):
            self.clipboard_enabled_toggle.setChecked(self.config_manager.get_clipboard_enabled())
        if hasattr(self, 'clipboard_auto_paste_toggle'):
            self.clipboard_auto_paste_toggle.setChecked(self.config_manager.get_clipboard_auto_paste())
        if hasattr(self, 'clipboard_history_limit_spin'):
            self.clipboard_history_limit_spin.setValue(self.config_manager.get_clipboard_history_limit())
        if hasattr(self, 'clipboard_scan_interval_combo'):
            idx = self.clipboard_scan_interval_combo.findData(
                self.config_manager.get_clipboard_foreground_scan_interval_ms()
            )
            if idx >= 0:
                self.clipboard_scan_interval_combo.setCurrentIndex(idx)

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

        # 预加载开关刷新（默认值统一来自 APP_DEFAULT_SETTINGS）
        if hasattr(self, 'preload_screenshot_toggle'):
            self.preload_screenshot_toggle.setChecked(self.config_manager.get_app_setting("preload_screenshot"))
        if hasattr(self, 'preload_toolbar_toggle'):
            self.preload_toolbar_toggle.setChecked(self.config_manager.get_app_setting("preload_toolbar"))
        if hasattr(self, 'preload_ocr_toggle'):
            self.preload_ocr_toggle.setChecked(self.config_manager.get_app_setting("preload_ocr"))
        if hasattr(self, 'preload_settings_toggle'):
            self.preload_settings_toggle.setChecked(self.config_manager.get_app_setting("preload_settings"))
        if hasattr(self, 'preload_clipboard_toggle'):
            self.preload_clipboard_toggle.setChecked(self.config_manager.get_app_setting("preload_clipboard"))

        # 截图信息面板行为
        if hasattr(self, 'info_hide_on_drag_toggle'):
            self.info_hide_on_drag_toggle.setChecked(
                self.config_manager.get_app_setting("screenshot_info_hide_on_drag")
            )

        # 外观设置
        if hasattr(self, '_ui_theme_combo'):
            from core.ui_theme import get_ui_theme
            index = self._ui_theme_combo.findData(get_ui_theme().mode.value)
            if index >= 0:
                self._ui_theme_combo.setCurrentIndex(index)

        if hasattr(self, '_ui_scale_combo'):
            from core.ui_scale import get_ui_scale
            index = self._ui_scale_combo.findData(get_ui_scale().percent)
            if index >= 0:
                self._ui_scale_combo.setCurrentIndex(index)

        if hasattr(self, '_dialog_scale_combo'):
            from core.ui_scale import get_dialog_scale
            index = self._dialog_scale_combo.findData(get_dialog_scale().percent)
            if index >= 0:
                self._dialog_scale_combo.setCurrentIndex(index)

        if hasattr(self, '_theme_color_btn'):
            from core.theme import get_theme
            from .page_appearance import _update_color_btn
            theme = get_theme()
            self._appearance_theme_color = QColor(theme.theme_color)
            mc = theme.mask_color
            self._appearance_mask_color = QColor(mc.red(), mc.green(), mc.blue())
            _update_color_btn(self._theme_color_btn, self._appearance_theme_color)
            _update_color_btn(self._mask_color_btn, self._appearance_mask_color)

        if hasattr(self, '_selection_border_combo'):
            from core.theme import get_theme
            index = self._selection_border_combo.findData(
                get_theme().selection_border_width
            )
            if index >= 0:
                self._selection_border_combo.setCurrentIndex(index)

        if hasattr(self, '_selection_handle_combo'):
            from core.theme import get_theme
            index = self._selection_handle_combo.findData(
                get_theme().selection_handle_style
            )
            if index >= 0:
                self._selection_handle_combo.setCurrentIndex(index)

        if hasattr(self, '_selection_handle_size_combo'):
            from core.theme import get_theme
            index = self._selection_handle_size_combo.findData(
                get_theme().selection_handle_size
            )
            if index >= 0:
                self._selection_handle_size_combo.setCurrentIndex(index)

        # 剪切板主题色同步（在别处改了主题色后打开设置，确保显示最新值）
        if hasattr(self, '_clip_theme_btn'):
            from settings import get_tool_settings_manager
            from .page_appearance import _apply_clip_theme_btn_style
            self._clip_theme_name = get_tool_settings_manager().get_clipboard_theme()
            _apply_clip_theme_btn_style(self._clip_theme_btn, self._clip_theme_name)
