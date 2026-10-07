# -*- coding: utf-8 -*-
"""GIF 模块公共小部件 / 工具函数"""

from __future__ import annotations

from PySide6.QtWidgets import QApplication, QPushButton, QMenu
from PySide6.QtCore import Qt, QPoint, Signal
from PySide6.QtGui import QIcon, QCursor, QAction

from core.ui_scale import scaled


def toolbar_style() -> str:
    """录制与回放共用透明控件，底纹由 paint_rounded_panel 绘制。"""
    return f"""
        .QWidget, .QLabel {{ background: transparent; border: none; color: #2a3243; }}
        QPushButton {{ border: none; border-radius: {scaled(7)}px;
                       color: #2a3243; font-size: {scaled(12)}px; padding: 0; }}
        QPushButton:hover {{ background: rgba(15,23,42,0.07); }}
        QPushButton:pressed {{ background: rgba(15,23,42,0.12); }}
        QPushButton:checked {{ background: rgba(74,124,184,0.16); }}
        QPushButton:disabled {{ color: #9aa3b1; }}
    """


def popup_menu(menu: QMenu, anchor, below=True):
    """异步显示按钮旁的浮层，靠屏幕边缘时自动翻转。"""
    menu.setStyleSheet(f"""
        QMenu {{ background: #fcfdff; color: #2a3243; border: 1px solid #dce2eb;
                 border-radius: {scaled(8)}px; padding: {scaled(6)}px; }}
        QMenu::item {{ padding: {scaled(7)}px {scaled(22)}px; font-size: {scaled(12)}px;
                      border-radius: {scaled(4)}px; }}
        QMenu::item:selected {{ background: #e6edf6; }}
        QMenu::item:disabled {{ color: #929baa; }}
        QMenu::separator {{ height: 1px; background: #e1e6ee; margin: {scaled(4)}px; }}
    """)
    size = menu.sizeHint()
    top = anchor.mapToGlobal(QPoint(0, 0))
    screen = QApplication.screenAt(top) or QApplication.primaryScreen()
    bounds = screen.availableGeometry()
    gap = scaled(5)
    down, up = top.y() + anchor.height() + gap, top.y() - size.height() - gap
    y = down if below else up
    if y < bounds.top() or y + size.height() > bounds.bottom() + 1:
        y = up if below else down
    x = max(bounds.left(), min(top.x(), bounds.right() + 1 - size.width()))
    y = max(bounds.top(), min(y, bounds.bottom() + 1 - size.height()))
    menu.aboutToHide.connect(menu.deleteLater)
    menu.popup(QPoint(x, y))


# ── 进度条通用样式 ─────────────────────────────────────
PROGRESS_BAR_STYLE = """
    QProgressBar {
        background: rgba(0, 0, 0, 45);
        border-radius: 7px;
        border: none;
    }
    QProgressBar::chunk {
        background: #2196F3;
        border-radius: 7px;
    }
"""


def svg_icon(name: str, size: int = 22) -> QIcon:
    """将 SVG 渲染到指定像素正方形，确保各图标显示大小一致。"""
    from core.resource_manager import ResourceManager
    return ResourceManager.get_icon(
        ResourceManager.get_icon_path(name), size=size
    )


class ClickMenuButton(QPushButton):
    """点击后弹出选项菜单，选中后更新文本并发射信号。"""

    option_selected = Signal(object)

    def __init__(self, options: list, default_index: int = 0, parent=None):
        """options: [(显示文本, 值), ...]"""
        super().__init__(parent)
        self._options = options
        self._current_index = default_index
        self._enabled_flag = True
        self._menu = None
        self.popup_below = True
        self._update_text()
        self.setCursor(QCursor(Qt.CursorShape.PointingHandCursor))
        self._apply_style()
        self.clicked.connect(self._show_menu)

    def _apply_style(self):
        color = "#333" if self._enabled_flag else "#bbb"
        self.setStyleSheet(
            f"QPushButton {{ font-size: {scaled(12)}px; color: {color};"
            f" padding: {scaled(1)}px {scaled(5)}px;"
            f" border: none; border-radius: {scaled(7)}px; background: transparent; }}"
            f"QPushButton:hover {{ background: rgba(15,23,42,0.07); }}"
        )

    def apply_scale(self):
        """按当前比例重挂样式（尺寸全靠样式表里的字号和内边距）"""
        self._apply_style()

    def _update_text(self):
        self.setText(self._options[self._current_index][0])

    def current_value(self):
        return self._options[self._current_index][1]

    def set_enabled(self, enabled: bool):
        if not enabled and self._menu is not None:
            self._menu.close()
        self._enabled_flag = enabled
        self.setEnabled(enabled)
        self._apply_style()

    def _show_menu(self):
        if not self.isEnabled():
            return
        if self._menu is not None:
            self._menu.close()
            return
        menu = QMenu(self)
        self._menu = menu
        menu.aboutToHide.connect(lambda: setattr(self, "_menu", None))
        for i, (text, value) in enumerate(self._options):
            act = QAction(text, menu)
            act.setCheckable(True)
            act.setChecked(i == self._current_index)
            act.triggered.connect(lambda checked, idx=i: self._on_select(idx))
            menu.addAction(act)
        popup_menu(menu, self, self.popup_below)

    def hideEvent(self, event):
        if self._menu is not None:
            self._menu.close()
        super().hideEvent(event)

    def _on_select(self, index: int):
        self._current_index = index
        self._update_text()
        self.option_selected.emit(self._options[index][1])
