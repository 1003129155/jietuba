# -*- coding: utf-8 -*-
"""
分组栏组件

独立的分组按钮栏 Widget，包含：
- 按钮布局构建（支持 left/right/top 三种位置）
- 侧栏键盘导航状态机
- 分组管理回调（增删改排序）
"""

from PySide6.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout, QBoxLayout, QPushButton, QMenu, QFrame,
    QApplication, QGraphicsOpacityEffect
)
from PySide6.QtCore import Qt, Signal, QTimer, QPoint, QPropertyAnimation, QEasingCurve
from PySide6.QtGui import QCursor, QAction

from typing import Optional, List
from .data_manager import ClipboardManager, Group
from .data_setting import ManageDialog, get_manage_dialog
from .data_controller import ClipboardController
from .themes import Theme
from .theme_styles import ThemeStyleGenerator


class GroupBar(QWidget):
    """
    分组栏组件 — 管理分组按钮布局和侧栏交互状态机。

    信号:
        group_switched(group_id)  — 用户切换了分组（None 表示"全部"）
        close_requested()         — 用户点了关闭按钮
        sidebar_entered()         — 进入侧栏导航模式（window 需清空列表选中）
        sidebar_exited(prev_id)   — 退出侧栏导航模式（window 可恢复之前选中）
        manage_groups_requested() — 请求打开分组管理对话框
        manage_items_requested()  — 请求打开内容管理对话框
    """

    group_switched = Signal(object)          # Optional[int]
    close_requested = Signal()
    sidebar_entered = Signal()
    sidebar_exited = Signal(object)          # Optional[int] — prev_item_id
    manage_groups_requested = Signal()
    manage_items_requested = Signal()

    def __init__(
        self,
        controller: ClipboardController,
        manager: ClipboardManager,
        theme: Theme,
        position: str = "right",
        parent: Optional[QWidget] = None,
    ):
        super().__init__(parent)
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.controller = controller
        self.manager = manager
        self.current_theme = theme
        self.position = position  # "right" / "left" / "top"

        # 分组按钮列表
        self.group_buttons: List[QPushButton] = []

        # 侧栏导航状态
        self._sidebar_mode = False
        self._sidebar_index = 0
        self._sidebar_prev_item_id: Optional[int] = None

        # ── 创建共享按钮（只创建一次，不随布局重建） ──
        self.close_btn = self._create_close_btn()
        self.clipboard_btn = self._create_clipboard_btn()
        self.add_group_btn = self._create_add_group_btn()

        # ── 构建布局 ──
        # GroupBar 自身就是实际显示的栏，保留 bar_widget 兼容外部调用。
        self.bar_widget: QWidget = self
        self.bar_layout: Optional[QBoxLayout] = None
        self.group_buttons_widget: QWidget = QWidget(self)
        self.group_buttons_widget.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.group_buttons_widget.setStyleSheet("background: transparent;")
        self.group_buttons_layout: Optional[QBoxLayout] = None

    # ================================================================
    #  公共 API — 供 window.py 调用
    # ================================================================

    @property
    def sidebar_mode(self) -> bool:
        return self._sidebar_mode

    def build(self, content_layout: QHBoxLayout, left_widget: QWidget, left_layout: QVBoxLayout):
        """首次构建分组栏并插入到 content_layout。"""
        self._content_layout = content_layout
        self._left_widget = left_widget
        self._left_layout = left_layout
        self._build_bar()
        QTimer.singleShot(0, self.refresh_buttons)

    def set_position(self, position: str):
        """切换分组栏位置（right/left/top）"""
        if position == self.position:
            return
        self.position = position
        self._build_bar()
        QTimer.singleShot(0, self.refresh_buttons)

    def set_theme(self, theme: Theme):
        """主题切换时刷新样式"""
        self.current_theme = theme
        sidebar_style = self._get_sidebar_btn_style()
        self.clipboard_btn.setStyleSheet(sidebar_style)
        for btn in self.group_buttons:
            btn.setStyleSheet(sidebar_style)
        self.add_group_btn.setStyleSheet(self._get_add_group_btn_style())

    def refresh_buttons(self):
        """刷新分组按钮；超出可用空间时折叠为溢出菜单"""
        self._clear_group_buttons_layout()
        self.clipboard_btn.setChecked(self.controller.current_group_id is None)

        is_top = self.position == "top"
        bar_size = self.bar_widget.width() if is_top else self.bar_widget.height()
        visible_groups, hidden_groups = self.controller.get_sidebar_overflow(bar_size, is_top=is_top)

        for group in visible_groups:
            btn = self._make_group_btn(group)
            self.group_buttons_layout.addWidget(btn)
            self.group_buttons.append(btn)

        self.group_buttons_layout.addSpacing(8)

        if hidden_groups:
            overflow_btn = QPushButton("···")
            overflow_btn.setFixedSize(34, 28)
            overflow_btn.setCursor(Qt.CursorShape.PointingHandCursor)
            overflow_btn.setFocusPolicy(Qt.FocusPolicy.NoFocus)
            overflow_btn.setToolTip(self.tr("More groups ({n})").format(n=len(hidden_groups)))
            overflow_btn.setStyleSheet("""
                QPushButton {
                    background: transparent;
                    color: #9CA3AF;
                    border: none;
                    font-size: 24px;
                    font-weight: 600;
                    padding: 0px;
                    letter-spacing: 1px;
                }
                QPushButton:hover {
                    color: #374151;
                }
            """)
            overflow_btn.clicked.connect(
                lambda _checked, hg=hidden_groups: self._show_overflow_group_menu(hg)
            )
            self.group_buttons_layout.addWidget(overflow_btn)

        self.group_buttons_layout.addWidget(self.add_group_btn)

    def enter_sidebar_mode(self, prev_item_id: Optional[int] = None):
        """进入侧栏导航模式"""
        buttons = self._get_sidebar_buttons()
        if not buttons:
            return
        self._sidebar_mode = True
        self._sidebar_prev_item_id = prev_item_id
        self.sidebar_entered.emit()
        self._sync_sidebar_index()
        self._set_sidebar_focus_button(buttons[self._sidebar_index])

    def exit_sidebar_mode(self):
        """退出侧栏导航模式，返回之前的 item_id"""
        self._sidebar_mode = False
        self._set_sidebar_focus_button(None)
        prev = self._sidebar_prev_item_id
        self._sidebar_prev_item_id = None
        self.sidebar_exited.emit(prev)

    def move_sidebar_selection(self, delta: int):
        """在侧栏内上下/左右移动选中"""
        buttons = self._get_sidebar_buttons()
        if not buttons:
            return
        self._sidebar_index = (self._sidebar_index + delta) % len(buttons)
        target = buttons[self._sidebar_index]
        self._set_sidebar_focus_button(target)
        target.click()

    def handle_top_group_switch(self, delta: int):
        """顶部模式左右键直接切换分组，不进入侧栏模式"""
        self._sync_sidebar_index()
        buttons = self._get_sidebar_buttons()
        if not buttons:
            return
        new_index = (self._sidebar_index + delta) % len(buttons)
        if new_index == self._sidebar_index:
            return
        self._sidebar_index = new_index
        buttons[self._sidebar_index].click()

    def clear_sidebar_focus(self):
        """清除侧栏焦点高亮（鼠标回到列表时调用）"""
        if not self._sidebar_mode:
            self._set_sidebar_focus_button(None)

    def switch_to_group(self, group_id: Optional[int]):
        """切换到指定分组（UI 同步 + 委托 controller）"""
        self.controller.switch_to_group(group_id)
        self.clipboard_btn.setChecked(group_id is None)
        for btn in self.group_buttons:
            btn.setChecked(btn.property("group_id") == group_id)
        self._sync_sidebar_index()

    # ================================================================
    #  按钮创建
    # ================================================================

    def _create_close_btn(self) -> QPushButton:
        btn = QPushButton("×")
        btn.setFixedSize(34, 34)
        btn.setCursor(Qt.CursorShape.PointingHandCursor)
        btn.setToolTip(self.tr("Close"))
        btn.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        btn.setStyleSheet("""
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
        btn.clicked.connect(self.close_requested.emit)
        return btn

    def _create_clipboard_btn(self) -> QPushButton:
        btn = QPushButton("📋")
        btn.setFixedSize(34, 34)
        btn.setCursor(Qt.CursorShape.PointingHandCursor)
        btn.setToolTip(self.tr("Clipboard History"))
        btn.setCheckable(True)
        btn.setChecked(True)
        btn.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        btn.setStyleSheet(self._get_sidebar_btn_style())
        btn.clicked.connect(lambda: self._on_sidebar_button_clicked(btn, None))
        return btn

    def _create_add_group_btn(self) -> QPushButton:
        btn = QPushButton("+")
        btn.setFixedSize(34, 34)
        btn.setCursor(Qt.CursorShape.PointingHandCursor)
        btn.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        btn.setStyleSheet(self._get_add_group_btn_style())
        btn.clicked.connect(self.manage_groups_requested.emit)
        return btn

    def _make_group_btn(self, group: Group) -> QPushButton:
        icon = group.icon if group.icon else "📁"
        btn = QPushButton(icon)
        btn.setFixedSize(34, 34)
        btn.setCursor(Qt.CursorShape.PointingHandCursor)
        btn.setToolTip(group.name)
        btn.setCheckable(True)
        btn.setChecked(self.controller.current_group_id == group.id)
        btn.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        btn.setStyleSheet(self._get_sidebar_btn_style())
        btn.setProperty("group_id", group.id)
        btn.clicked.connect(lambda checked, b=btn, gid=group.id: self._on_sidebar_button_clicked(b, gid))
        btn.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        btn.customContextMenuRequested.connect(
            lambda pos, b=btn, gid=group.id: self._show_group_context_menu(b, gid, pos)
        )
        return btn

    # ================================================================
    #  布局构建
    # ================================================================

    def _clear_bar_layout(self):
        if self.bar_layout is None:
            return
        preserve_widgets = {
            self.close_btn,
            self.clipboard_btn,
            self.add_group_btn,
            self.group_buttons_widget,
        }
        while self.bar_layout.count():
            item = self.bar_layout.takeAt(0)
            widget = item.widget()
            if widget is not None and widget not in preserve_widgets:
                widget.deleteLater()

    def _remove_from_host_layouts(self):
        self._content_layout.removeWidget(self)
        self._left_layout.removeWidget(self)

    def _build_bar(self):
        """根据 self.position 构建（或重建）分组按钮栏"""
        pos = self.position
        is_top = pos == "top"

        self._remove_from_host_layouts()

        direction = (
            QBoxLayout.Direction.LeftToRight
            if is_top else
            QBoxLayout.Direction.TopToBottom
        )
        if self.bar_layout is None:
            self.bar_layout = QBoxLayout(direction, self)
        else:
            self._clear_bar_layout()
            self.bar_layout.setDirection(direction)

        if is_top:
            self.setMinimumHeight(40)
            self.setMaximumHeight(40)
            self.setMinimumWidth(0)
            self.setMaximumWidth(16777215)
            self.setStyleSheet("QWidget { background: #FAFAFA; border-bottom: 1px solid #E0E0E0; }")
            self.bar_layout.setContentsMargins(2, 2, 2, 2)
            self.bar_layout.setSpacing(4)
        else:
            self.setMinimumWidth(40)
            self.setMaximumWidth(40)
            self.setMinimumHeight(0)
            self.setMaximumHeight(16777215)
            border_side = "border-right" if pos == "left" else "border-left"
            self.setStyleSheet(f"QWidget {{ background: #FAFAFA; {border_side}: 1px solid #E0E0E0; }}")
            self.bar_layout.setContentsMargins(2, 8, 2, 8)
            self.bar_layout.setSpacing(4)

        if self.group_buttons_layout is None:
            self.group_buttons_layout = QBoxLayout(direction, self.group_buttons_widget)
            self.group_buttons_layout.setContentsMargins(0, 0, 0, 0)
            self.group_buttons_layout.setSpacing(4)
        else:
            self.group_buttons_layout.setDirection(direction)

        # --- 添加按钮到布局 ---
        if is_top:
            self.bar_layout.addWidget(self.clipboard_btn)
            sep = QFrame(); sep.setFixedWidth(1); sep.setStyleSheet("background: #E0E0E0;")
            self.bar_layout.addWidget(sep)
            self.bar_layout.addWidget(self.group_buttons_widget)
            self.group_buttons_layout.addWidget(self.add_group_btn)
            self.bar_layout.addStretch()
            self.bar_layout.addWidget(self.close_btn)
        else:
            self.bar_layout.addWidget(self.close_btn)
            sep = QFrame(); sep.setFixedHeight(1); sep.setStyleSheet("background: #E0E0E0;")
            self.bar_layout.addWidget(sep)
            self.bar_layout.addWidget(self.clipboard_btn)
            self.bar_layout.addWidget(self.group_buttons_widget)
            self.group_buttons_layout.addWidget(self.add_group_btn)
            self.bar_layout.addStretch()

        # --- 插入到 content_layout ---
        if pos == "left":
            self._content_layout.insertWidget(0, self)
        elif is_top:
            self._content_layout.removeWidget(self._left_widget)
            self._left_layout.insertWidget(0, self)
            self._content_layout.addWidget(self._left_widget, 1)
        else:
            self._content_layout.addWidget(self)

    # ================================================================
    #  样式
    # ================================================================

    def _get_sidebar_btn_style(self) -> str:
        theme = self.current_theme.colors
        return f"""
            QPushButton {{
                background: transparent;
                color: {theme.text_secondary};
                border: 2px solid transparent;
                font-size: 20px;
                border-radius: 4px;
                padding: 0px;
            }}
            QPushButton:hover {{
                background: {theme.bg_hover};
            }}
            QPushButton:checked {{
                background: {theme.bg_selected};
                color: {theme.accent_primary};
                border: 2px solid {theme.error};
            }}
            QToolTip {{
                background: {theme.bg_primary};
                color: {theme.text_primary};
                border: 1px solid {theme.border_primary};
                border-radius: 4px;
                padding: 4px 8px;
                font-size: 12px;
            }}
        """

    def _get_add_group_btn_style(self) -> str:
        theme = self.current_theme.colors
        if self.current_theme.name == "dark":
            bg_color = "rgba(60, 60, 60, 0.7)"
            hover_bg = "rgba(76, 175, 80, 0.2)"
        else:
            bg_color = "rgba(255, 255, 255, 0.55)"
            hover_bg = "#E8F5E9"
        return f"""
            QPushButton {{
                background-color: {bg_color};
                color: {theme.success};
                border: 2px dashed {theme.success};
                border-radius: 4px;
                font-size: 20px;
                font-weight: bold;
                padding: 0px;
            }}
            QPushButton:hover {{
                background: {hover_bg};
            }}
        """

    def _get_menu_style(self) -> str:
        return ThemeStyleGenerator(self.current_theme).generate_menu_style()

    # ================================================================
    #  侧栏导航状态机
    # ================================================================

    def _get_sidebar_buttons(self) -> List[QPushButton]:
        return [self.clipboard_btn] + self.group_buttons

    def _on_sidebar_button_clicked(self, target: QPushButton, group_id: Optional[int]):
        if not self._sidebar_mode:
            self.sidebar_entered.emit()
        self._sidebar_mode = True
        buttons = self._get_sidebar_buttons()
        if target in buttons:
            self._sidebar_index = buttons.index(target)
        self._set_sidebar_focus_button(target)
        self.group_switched.emit(group_id)

    def _sync_sidebar_index(self):
        if self.controller.current_group_id is None:
            self._sidebar_index = 0
            return
        for i, btn in enumerate(self.group_buttons, start=1):
            if btn.property("group_id") == self.controller.current_group_id:
                self._sidebar_index = i
                return
        self._sidebar_index = 0

    def _set_sidebar_focus_button(self, target: Optional[QPushButton]):
        for btn in self._get_sidebar_buttons():
            btn.setProperty("sidebar_focus", btn is target)
            btn.style().unpolish(btn)
            btn.style().polish(btn)
            anim = getattr(btn, "_sidebar_focus_anim", None)
            if btn is not target and anim is not None:
                anim.stop()
                if isinstance(btn.graphicsEffect(), QGraphicsOpacityEffect):
                    btn.graphicsEffect().setOpacity(1.0)
        if target is not None and self._sidebar_mode and self.position != "top":
            self._play_sidebar_focus_animation(target)

    def _play_sidebar_focus_animation(self, target: QPushButton):
        effect = target.graphicsEffect()
        if not isinstance(effect, QGraphicsOpacityEffect):
            effect = QGraphicsOpacityEffect(target)
            effect.setOpacity(1.0)
            target.setGraphicsEffect(effect)
        anim = getattr(target, "_sidebar_focus_anim", None)
        if anim is None:
            anim = QPropertyAnimation(effect, b"opacity", target)
            anim.setDuration(1200)
            anim.setLoopCount(-1)
            anim.setEasingCurve(QEasingCurve.Type.InOutQuad)
            anim.setKeyValueAt(0.0, 1.0)
            anim.setKeyValueAt(0.5, 0.3)
            anim.setKeyValueAt(1.0, 1.0)
            target._sidebar_focus_anim = anim
        if anim.state() != anim.State.Running:
            anim.start()

    # ================================================================
    #  溢出菜单
    # ================================================================

    def _show_overflow_group_menu(self, hidden_groups: list):
        if not hidden_groups:
            return
        menu = QMenu(self)
        menu.setStyleSheet(self._get_menu_style())
        for group in hidden_groups:
            icon = group.icon if group.icon else "📁"
            label = f"{icon}  {group.name}"
            action = QAction(label, self)
            action.setCheckable(True)
            action.setChecked(group.id == self.controller.current_group_id)
            action.triggered.connect(lambda checked, gid=group.id: self.group_switched.emit(gid))
            menu.addAction(action)
        menu.exec(QCursor.pos())

    def _clear_group_buttons_layout(self):
        while self.group_buttons_layout.count():
            item = self.group_buttons_layout.takeAt(0)
            widget = item.widget()
            if widget:
                if widget is self.add_group_btn:
                    continue
                widget.deleteLater()
        self.group_buttons.clear()

    # ================================================================
    #  分组右键菜单 + 管理操作
    # ================================================================

    def _translate_group_action_label(self, action_key: str, fallback_label: str) -> str:
        label_map = {
            "edit_group": self.tr("Edit"),
            "move_group_up": self.tr("Move Up"),
            "move_group_down": self.tr("Move Down"),
            "delete_group": self.tr("Delete Group"),
        }
        return label_map.get(action_key, fallback_label)

    def _show_group_context_menu(self, btn, group_id: int, pos):
        actions_data = self.controller.build_group_context_menu_data(group_id)
        menu = QMenu(self)
        menu.setStyleSheet(self._get_menu_style())

        _group_ctx_handlers = {
            "edit_group":      lambda: self._edit_group(group_id),
            "move_group_up":   lambda: self._move_group_order(group_id, -1),
            "move_group_down": lambda: self._move_group_order(group_id, 1),
            "delete_group":    lambda: self._delete_group(group_id),
        }
        for ad in actions_data:
            if ad.is_separator:
                menu.addSeparator()
            else:
                act = menu.addAction(self._translate_group_action_label(ad.key, ad.label))
                act.setEnabled(ad.enabled)
                handler = _group_ctx_handlers.get(ad.key)
                if handler:
                    act.triggered.connect(handler)
        menu.exec(btn.mapToGlobal(pos))

    def _delete_group(self, group_id: int):
        if self.controller.delete_group(group_id, parent_widget=self):
            self.refresh_buttons()

    def _move_group_order(self, group_id: int, direction: int):
        if self.controller.move_group_order(group_id, direction):
            self.refresh_buttons()

    def _edit_group(self, group_id: int):
        self.controller.open_manage_dialog_for_group(
            group_id,
            group_added_callback=self.refresh_buttons,
            data_changed_callback=self._on_manage_data_changed,
        )

    def _on_manage_data_changed(self):
        """管理窗口数据变更回调 — 向上传播给 window"""
        # window.py 会通过信号或直接引用来处理
        parent = self.parent()
        if parent and hasattr(parent, '_on_manage_data_changed'):
            parent._on_manage_data_changed()
