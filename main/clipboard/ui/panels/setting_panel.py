# -*- coding: utf-8 -*-
"""
设置面板
右下角齿轮按钮的设置菜单
"""

from PySide6.QtWidgets import (
    QHBoxLayout, QPushButton, QMenu, QWidget, QWidgetAction
)
from PySide6.QtCore import Qt

from typing import Optional, Callable

from core.ui_scale import scaled
from ui.fluent_lite import LineEdit

from ..theme.themes import PRESET_THEME_SWATCHES
from ..menus.submenu_position import avoid_submenu_overlap


# 多选粘贴的分隔符预设：(菜单文字, 分隔符)
MULTI_PASTE_SEPARATORS = (
    ("New Line", "\n"),
    ("Blank Line", "\n\n"),
    ("Space", " "),
    ("Tab", "\t"),
    ("Comma", ", "),
    ("No Separator", ""),
)


def escape_separator(text: str) -> str:
    """分隔符显示在输入框里：换行、制表符写成 \\n、\\t。"""
    return text.replace("\\", "\\\\").replace("\n", "\\n").replace("\t", "\\t")


def unescape_separator(text: str) -> str:
    out = []
    index = 0
    while index < len(text):
        pair = text[index:index + 2]
        if pair in ("\\n", "\\t", "\\\\"):
            out.append({"\\n": "\n", "\\t": "\t", "\\\\": "\\"}[pair])
            index += 2
        else:
            out.append(text[index])
            index += 1
    return "".join(out)


def _add_multi_paste_menu(menu: QMenu, menu_style: str, tr: Callable, separator: str, order: str,
                          keep_merged: bool, on_set_separator: Callable, on_set_order: Callable,
                          on_toggle_keep_merged: Callable):
    multi_menu = menu.addMenu(tr("Multi-Select Paste"))
    multi_menu.setStyleSheet(menu_style)

    separator_menu = multi_menu.addMenu(tr("Separator"))
    separator_menu.setStyleSheet(menu_style)
    for label, value in MULTI_PASTE_SEPARATORS:
        act = separator_menu.addAction(tr(label))
        act.setCheckable(True)
        act.setChecked(separator == value)
        act.triggered.connect(lambda _c, v=value: on_set_separator(v))

    # 自定义：输入框里 \n 是换行、\t 是制表符，回车生效
    custom = QWidget(separator_menu)
    layout = QHBoxLayout(custom)
    layout.setContentsMargins(scaled(8), scaled(4), scaled(8), scaled(4))
    custom_edit = LineEdit(custom)
    custom_edit.setPlaceholderText(tr("Custom, \\n for a new line"))
    custom_edit.setMinimumWidth(scaled(150))
    if separator not in {value for _label, value in MULTI_PASTE_SEPARATORS}:
        custom_edit.setText(escape_separator(separator))

    def _apply_custom():
        on_set_separator(unescape_separator(custom_edit.text()))
        menu.close()

    custom_edit.returnPressed.connect(_apply_custom)
    layout.addWidget(custom_edit)
    custom_action = QWidgetAction(separator_menu)
    custom_action.setDefaultWidget(custom)
    separator_menu.addAction(custom_action)

    order_menu = multi_menu.addMenu(tr("Order"))
    order_menu.setStyleSheet(menu_style)
    for key, label in (("oldest_first", tr("First Selected First")), ("newest_first", tr("Last Selected First"))):
        act = order_menu.addAction(label)
        act.setCheckable(True)
        act.setChecked(order == key)
        act.triggered.connect(lambda _c, k=key: on_set_order(k))

    keep_act = multi_menu.addAction(tr("Record Content"))
    keep_act.setCheckable(True)
    keep_act.setChecked(keep_merged)
    keep_act.triggered.connect(on_toggle_keep_merged)


# ──────────────────────────────────────────────
# 设置菜单（原 window.py _show_main_menu 逻辑）
# ──────────────────────────────────────────────


def show_setting_menu(
    parent,
    *,
    menu_style: str,
    tr: Callable,
    # 当前状态
    paste_with_html: bool,
    auto_paste: bool,
    close_after_paste: bool,
    move_to_top: bool,
    show_metadata: bool,
    preserve_search: bool,
    window_opacity: int,
    current_font_size: int,
    current_image_size: str = "small",
    current_theme_name: str,
    current_group_bar_position: str = "right",
    opacity_options: list,
    font_size_options: list,
    # 回调
    on_toggle_paste_html: Callable,
    on_toggle_auto_paste: Callable,
    on_toggle_close_after_paste: Callable,
    on_toggle_move_to_top: Callable,
    on_toggle_show_metadata: Callable,
    on_toggle_preserve_search: Callable,
    on_set_opacity: Callable,
    on_set_font_size: Callable,
    on_set_image_size: Optional[Callable] = None,
    on_set_theme: Callable,
    on_add_item: Callable,
    on_set_group_bar_position: Optional[Callable] = None,
    # 多选粘贴
    multi_paste_separator: str = "\n",
    multi_paste_order: str = "oldest_first",
    multi_paste_keep_merged: bool = False,
    on_set_multi_paste_separator: Optional[Callable] = None,
    on_set_multi_paste_order: Optional[Callable] = None,
    on_toggle_multi_paste_keep_merged: Optional[Callable] = None,
    # 弹出锚点（全局坐标 QPoint）
    anchor_pos,
):
    """
    构建并弹出设置菜单。
    所有业务状态和回调由调用方（window.py）传入，本函数只做 UI 组装。
    """
    menu = QMenu(parent)
    menu.setStyleSheet(menu_style)

    # ── 开关项 ──
    def _toggle_action(label, is_checked, callback):
        act = menu.addAction(tr(label))
        act.setCheckable(True)
        act.setChecked(is_checked)
        act.triggered.connect(callback)

    _toggle_action("Paste with Format", paste_with_html, on_toggle_paste_html)
    _toggle_action("Auto Paste After Selection", auto_paste, on_toggle_auto_paste)
    _toggle_action("Close After Paste", close_after_paste, on_toggle_close_after_paste)
    _toggle_action("Move to Top After Paste", move_to_top, on_toggle_move_to_top)
    _toggle_action("Show Time and Source", show_metadata, on_toggle_show_metadata)
    _toggle_action("Preserve Search on Reopen", preserve_search, on_toggle_preserve_search)

    if on_set_multi_paste_separator is not None:
        _add_multi_paste_menu(
            menu, menu_style, tr, multi_paste_separator, multi_paste_order, multi_paste_keep_merged,
            on_set_multi_paste_separator, on_set_multi_paste_order, on_toggle_multi_paste_keep_merged,
        )

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

    # ── 图片大小子菜单 ──
    if on_set_image_size is not None:
        image_menu = menu.addMenu(tr("Image Size"))
        image_menu.setStyleSheet(menu_style)
        for key, label in [("small", tr("Small")), ("medium", tr("Medium")), ("large", tr("Large"))]:
            act = image_menu.addAction(label)
            act.setCheckable(True)
            act.setChecked(current_image_size == key)
            act.triggered.connect(lambda _c, k=key: on_set_image_size(k))

    # ── 主题子菜单 ──
    theme_menu = menu.addMenu(tr("Theme"))
    theme_menu.setStyleSheet(menu_style)
    theme_buttons: list = []

    def _update_theme_btns(selected: str):
        for name, btn, accent, bg in theme_buttons:
            is_sel = name == selected
            btn.setText("✓" if is_sel else "")
            text_color = "#FFFFFF" if name == "dark" else "#333333"
            bw = scaled(2) if is_sel else 1
            bc = accent if is_sel else "#CCCCCC"
            btn.setStyleSheet(f"""
                QPushButton {{
                    background: qlineargradient(x1:0,y1:0,x2:1,y2:0,
                        stop:0 {bg}, stop:0.5 {bg},
                        stop:0.5 {accent}, stop:1 {accent});
                    border: {bw}px solid {bc};
                    border-radius: {scaled(4)}px;
                    color: {text_color};
                    font-size: {scaled(12)}px; font-weight: bold;
                    text-align: left; padding-left: {scaled(6)}px;
                }}
                QPushButton:hover {{ border: {scaled(2)}px solid {accent}; }}
            """)

    def _on_theme_click(name: str):
        on_set_theme(name)
        _update_theme_btns(name)
        theme_menu.close()

    for theme_name, (accent_color, bg_color) in PRESET_THEME_SWATCHES.items():
        wa = QWidgetAction(theme_menu)
        btn = QPushButton()
        btn.setFixedSize(scaled(120), scaled(28))
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
    avoid_submenu_overlap(menu)
    menu.popup(popup_pos)
    return menu


__all__ = ["show_setting_menu"]
