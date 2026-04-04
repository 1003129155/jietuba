# -*- coding: utf-8 -*-
"""GIF 模块公共小部件 / 工具函数"""

from __future__ import annotations

from PySide6.QtWidgets import QPushButton, QMenu
from PySide6.QtCore import Qt, QPoint, Signal, QSize
from PySide6.QtGui import QIcon, QCursor, QAction


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
        self._update_text()
        self.setCursor(QCursor(Qt.CursorShape.PointingHandCursor))
        self._apply_style()
        self.clicked.connect(self._show_menu)

    def _apply_style(self):
        color = "#333" if self._enabled_flag else "#bbb"
        bg = "#f5f5f5" if self._enabled_flag else "#ebebeb"
        self.setStyleSheet(
            f"QPushButton {{ font-size: 12px; color: {color}; padding: 1px 5px;"
            f" border: 1px solid #ddd; border-radius: 3px; background: {bg}; }}"
            f"QPushButton:hover {{ background: #e8e8e8; }}"
        )

    def _update_text(self):
        self.setText(self._options[self._current_index][0])

    def current_value(self):
        return self._options[self._current_index][1]

    def set_enabled(self, enabled: bool):
        self._enabled_flag = enabled
        self.setEnabled(enabled)
        self._apply_style()

    def _show_menu(self):
        menu = QMenu(self)
        menu.setStyleSheet("""
            QMenu {
                background: white;
                border: 1px solid #ddd;
                border-radius: 4px;
                padding: 2px 0;
            }
            QMenu::item {
                padding: 4px 16px;
                font-size: 12px;
                color: #333;
            }
            QMenu::item:selected { background: #f0f0f0; }
        """)
        for i, (text, value) in enumerate(self._options):
            act = QAction(text, menu)
            if i == self._current_index:
                font = act.font()
                font.setBold(True)
                act.setFont(font)
            act.triggered.connect(lambda checked, idx=i: self._on_select(idx))
            menu.addAction(act)
        pos = self.mapToGlobal(QPoint(0, self.height()))
        menu.exec(pos)

    def _on_select(self, index: int):
        self._current_index = index
        self._update_text()
        self.option_selected.emit(self._options[index][1])
 