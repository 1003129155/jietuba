# -*- coding: utf-8 -*-
"""
设置面板
右下角齿轮按钮的设置菜单
"""

from PySide6.QtWidgets import (
    QPushButton, QMenu, QWidgetAction
)
from PySide6.QtCore import Qt
from PySide6.QtGui import QAction

from typing import Optional, Callable
from .themes import get_theme_manager


# ──────────────────────────────────────────────
# 设置菜单（原 window.py _show_main_menu 逻辑）
# ──────────────────────────────────────────────

# 主题颜色表：(name, accent, bg)
_THEME_COLORS = [
    ("light",  "#DDE3E9", "#FFFFFF"),
    ("dark",   "#1D1F20", "#1E1E1E"),
    ("blue",   "#2196F3", "#9AD1F8"),
    ("green",  "#4CAF50", "#B8F3BD"),
    ("pink",   "#E91E63", "#FA9BBB"),
    ("purple", "#9C27B0", "#D471E4"),
    ("orange", "#FF9800", "#F5C880"),
]


def show_setting_menu(
    parent,
    *,
    menu_style: str,
    tr: Callable,
    # 当前状态
    current_filter_index: int,
    paste_with_html: bool,
    auto_paste: bool,
    move_to_top: bool,
    show_metadata: bool,
    preserve_search: bool,
    window_opacity: int,
    current_font_size: int,
    current_theme_name: str,
    current_group_bar_position: str = "right",
    opacity_options: list,
    font_size_options: list,
    # 回调
    on_set_filter: Callable,
    on_toggle_paste_html: Callable,
    on_toggle_auto_paste: Callable,
    on_toggle_move_to_top: Callable,
    on_toggle_show_metadata: Callable,
    on_toggle_preserve_search: Callable,
    on_set_opacity: Callable,
    on_set_font_size: Callable,
    on_set_theme: Callable,
    on_add_item: Callable,
    on_set_group_bar_position: Optional[Callable] = None,
    # 弹出锚点（全局坐标 QPoint）
    anchor_pos,
):
    """
    构建并弹出设置菜单。
    所有业务状态和回调由调用方（window.py）传入，本函数只做 UI 组装。
    """
    menu = QMenu(parent)
    menu.setStyleSheet(menu_style)

    # ── 筛选类型 ──
    filter_menu = menu.addMenu(tr("Filter Type"))
    filter_menu.setStyleSheet(menu_style)
    for i, name in enumerate([tr("All"), tr("Text"), tr("Image"), tr("File")]):
        act = filter_menu.addAction(name)
        act.setCheckable(True)
        act.setChecked(current_filter_index == i)
        act.triggered.connect(lambda _c, idx=i: on_set_filter(idx))

    menu.addSeparator()

    # ── 开关项 ──
    def _toggle_action(label, is_checked, callback):
        act = menu.addAction(tr(label))
        act.setCheckable(True)
        act.setChecked(is_checked)
        act.triggered.connect(callback)

    _toggle_action("Paste with Format",        paste_with_html,  on_toggle_paste_html)
    _toggle_action("Auto Paste After Selection", auto_paste,      on_toggle_auto_paste)
    _toggle_action("Move to Top After Paste",    move_to_top,     on_toggle_move_to_top)
    _toggle_action("Show Time and Source",          show_metadata,   on_toggle_show_metadata)
    _toggle_action("Preserve Search on Reopen",     preserve_search, on_toggle_preserve_search)

    menu.addSeparator()

    # ── 透明度子菜单 ──
    opacity_menu = menu.addMenu(tr("Window Opacity"))
    opacity_menu.setStyleSheet(menu_style)
    for percent in opacity_options:
        label = tr("Opaque") if percent == 0 else f"{percent}%"
        act = opacity_menu.addAction(label)
        act.setCheckable(True)
        act.setChecked(window_opacity == percent)
        act.triggered.connect(lambda _c, p=percent: on_set_opacity(p))

    # ── 字体大小子菜单 ──
    font_menu = menu.addMenu(tr("Font Size"))
    font_menu.setStyleSheet(menu_style)
    for size in font_size_options:
        act = font_menu.addAction(f"{size}px")
        act.setCheckable(True)
        act.setChecked(current_font_size == size)
        act.triggered.connect(lambda _c, s=size: on_set_font_size(s))

    # ── 主题子菜单 ──
    theme_menu = menu.addMenu(tr("Theme"))
    theme_menu.setStyleSheet(menu_style)
    theme_buttons: list = []

    def _update_theme_btns(selected: str):
        for name, btn, accent, bg in theme_buttons:
            is_sel = name == selected
            btn.setText("✓" if is_sel else "")
            text_color = "#FFFFFF" if name == "dark" else "#333333"
            bw = 2 if is_sel else 1
            bc = accent if is_sel else "#CCCCCC"
            btn.setStyleSheet(f"""
                QPushButton {{
                    background: qlineargradient(x1:0,y1:0,x2:1,y2:0,
                        stop:0 {bg}, stop:0.5 {bg},
                        stop:0.5 {accent}, stop:1 {accent});
                    border: {bw}px solid {bc};
                    border-radius: 4px;
                    color: {text_color};
                    font-size: 12px; font-weight: bold;
                    text-align: left; padding-left: 6px;
                }}
                QPushButton:hover {{ border: 2px solid {accent}; }}
            """)

    def _on_theme_click(name: str):
        on_set_theme(name)
        _update_theme_btns(name)
        theme_menu.close()

    for theme_name, accent_color, bg_color in _THEME_COLORS:
        wa = QWidgetAction(theme_menu)
        btn = QPushButton()
        btn.setFixedSize(120, 28)
        btn.setCursor(Qt.CursorShape.PointingHandCursor)
        btn.clicked.connect(lambda _c, n=theme_name: _on_theme_click(n))
        wa.setDefaultWidget(btn)
        theme_menu.addAction(wa)
        theme_buttons.append((theme_name, btn, accent_color, bg_color))

    _update_theme_btns(current_theme_name)

    # ── 分组栏位置子菜单 ──
    if on_set_group_bar_position is not None:
        bar_pos_menu = menu.addMenu(tr("Group Bar Position"))
        bar_pos_menu.setStyleSheet(menu_style)
        for key, label in [("right", tr("▶ Right")), ("left", tr("◀ Left")), ("top", tr("▲ Top"))]:
            act = bar_pos_menu.addAction(label)
            act.setCheckable(True)
            act.setChecked(current_group_bar_position == key)
            act.triggered.connect(lambda _c, k=key: on_set_group_bar_position(k))

    menu.addSeparator()

    # ── 添加内容 ──
    add_act = menu.addAction(tr("Add Content"))
    add_act.triggered.connect(on_add_item)

    # 在锚点上方弹出（非阻塞，支持 toggle）
    from PySide6.QtCore import QPoint as _QPoint
    popup_pos = _QPoint(anchor_pos.x(), anchor_pos.y() - menu.sizeHint().height())
    menu.popup(popup_pos)
    return menu
 