# -*- coding: utf-8 -*-
"""剪贴板弹出窗口跟随「工具栏与面板缩放」：样式、列表行、分组栏、预览和快速编辑一起放大。"""
from types import SimpleNamespace

import pytest
from PySide6.QtCore import QPoint, QRect, QSize
from PySide6.QtWidgets import (
    QHBoxLayout, QListWidget, QListWidgetItem, QStyleOptionViewItem, QVBoxLayout, QWidget,
)

from clipboard.controllers.clipboard_controller import calc_sidebar_capacity, calc_topbar_capacity
from clipboard.core import ClipboardItem
from clipboard.ui.theme.theme_styles import ThemeStyleGenerator
from clipboard.ui.theme.themes import THEME_LIGHT
from clipboard.ui.widgets.group_bar import GroupBar
from clipboard.ui.widgets.item_delegate import ROLE_ITEM_DATA, ClipboardItemDelegate
from clipboard.ui.widgets.preview_popup import PreviewPopup
from clipboard.ui.widgets.quick_edit_popup import QuickEditPopup
from core.ui_scale import get_ui_scale


@pytest.fixture(autouse=True)
def restore_scale():
    """比例是进程级单例，用完还原，免得影响后面的测试"""
    manager = get_ui_scale()
    before = manager.percent
    before_config = manager._config_manager
    manager._config_manager = None
    yield manager
    manager.set_percent(before)
    manager._config_manager = before_config


def test_popup_styles_scale_sizes_but_keep_hairlines(restore_scale):
    restore_scale.set_percent(150)
    menu = ThemeStyleGenerator(THEME_LIGHT).generate_menu_style()
    assert "padding: 12px 30px;" in menu
    assert "border: 1px solid" in menu
    assert "height: 1px;" in menu


def test_list_rows_grow_with_the_chosen_font_size(qapp, restore_scale):
    view = QListWidget()
    row = QListWidgetItem()
    row.setData(ROLE_ITEM_DATA, ClipboardItem(id=1, content="abc", content_type="text"))
    view.addItem(row)
    delegate = ClipboardItemDelegate(view, THEME_LIGHT, display_lines=17, show_metadata=True)
    index = view.indexFromItem(row)

    restore_scale.set_percent(100)
    delegate.apply_scale()
    assert delegate.sizeHint(QStyleOptionViewItem(), index).height() == int(17 * 1.4) + 2 + 16
    restore_scale.set_percent(150)
    delegate.apply_scale()
    assert delegate._font_cache["content"].pixelSize() == 26
    assert delegate.sizeHint(QStyleOptionViewItem(), index).height() == int(26 * 1.4) + 2 + 24
    view.deleteLater()


def test_group_capacity_is_counted_in_scaled_slots(restore_scale):
    restore_scale.set_percent(100)
    tall, wide = calc_sidebar_capacity(600), calc_topbar_capacity(600)
    restore_scale.set_percent(150)
    assert calc_sidebar_capacity(900) == tall
    assert calc_topbar_capacity(900) == wide


def test_group_bar_rebuilds_at_the_new_size(qapp, restore_scale):
    host = QWidget()
    content = QHBoxLayout(host)
    left = QWidget()
    left_layout = QVBoxLayout(left)
    content.addWidget(left)
    controller = SimpleNamespace(current_group_id=None,
                                 get_sidebar_overflow=lambda _size, is_top=False: ([], []))
    bar = GroupBar(controller, None, THEME_LIGHT, "right", host)
    bar.build(content, left, left_layout)
    qapp.processEvents()

    restore_scale.set_percent(150)
    bar.apply_scale()
    qapp.processEvents()
    assert bar.close_btn.size() == QSize(51, 51)
    assert bar.add_group_btn.size() == QSize(51, 51)
    assert bar.minimumWidth() == bar.maximumWidth() == 60
    assert "font-size: 30px" in bar.clipboard_btn.styleSheet()
    host.deleteLater()


def test_preview_limits_follow_panel_scale(qapp, restore_scale):
    popup = PreviewPopup()
    restore_scale.set_percent(150)
    popup.apply_scale()
    assert popup.content_widget.maximumSize() == QSize(750, 600)
    assert "font-size: 20px" in popup.content_widget.styleSheet()
    popup.deleteLater()


def test_quick_edit_opens_at_the_current_scale(qapp, restore_scale):
    parent = QWidget()
    popup = QuickEditPopup(parent)
    restore_scale.set_percent(150)
    popup.open_for(ClipboardItem(id=7, content="abc", content_type="text"),
                   QPoint(100, 100), QRect(0, 0, 50, 50), THEME_LIGHT)
    assert popup.size() == QSize(630, 360)
    assert "font-size: 20px" in popup.styleSheet()
    popup.hide()
    parent.deleteLater()
