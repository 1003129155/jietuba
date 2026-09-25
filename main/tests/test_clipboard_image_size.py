# -*- coding: utf-8 -*-
"""剪贴板图片条目高度档位：小/中/大 分别占 1/1.5/2 行。"""

import base64

import pytest
from PySide6.QtCore import QBuffer, QByteArray, QIODevice, QRect
from PySide6.QtGui import QColor, QImage, QPainter
from PySide6.QtWidgets import QListWidget, QListWidgetItem, QStyleOptionViewItem

from clipboard.core import ClipboardItem
from clipboard.ui.theme.themes import get_theme_manager
from clipboard.ui.widgets.item_delegate import ROLE_ITEM_DATA, ClipboardItemDelegate
from settings.tool_settings import ToolSettingsManager


def _data_url(width, height):
    image = QImage(width, height, QImage.Format.Format_ARGB32)
    image.fill(QColor("red"))
    data = QByteArray()
    buffer = QBuffer(data)
    buffer.open(QIODevice.OpenModeFlag.WriteOnly)
    image.save(buffer, "PNG")
    return "data:image/png;base64," + base64.b64encode(bytes(data)).decode()


@pytest.fixture
def make_index(qapp):
    lists = []

    def _make(item):
        widget = QListWidget()
        lists.append(widget)
        row = QListWidgetItem()
        row.setData(ROLE_ITEM_DATA, item)
        widget.addItem(row)
        return widget.model().index(0, 0)

    yield _make
    for widget in lists:
        widget.deleteLater()


def _item(content_type, thumbnail=None):
    return ClipboardItem(id=1, content="x", content_type=content_type, thumbnail=thumbnail)


def _height(delegate, index):
    return delegate.sizeHint(QStyleOptionViewItem(), index).height()


@pytest.mark.parametrize("size, span", [("small", 1), ("medium", 1.5), ("large", 2)])
def test_image_row_spans_that_many_text_rows(make_index, size, span):
    text_index = make_index(_item("text"))
    image_index = make_index(_item("image", _data_url(64, 32)))
    delegate = ClipboardItemDelegate(display_lines=17, image_size=size)

    text_height = _height(delegate, text_index)

    assert _height(delegate, image_index) == max(int(text_height * span), 42)


def test_changing_size_only_affects_image_rows(make_index):
    text_index = make_index(_item("text"))
    image_index = make_index(_item("image", _data_url(64, 32)))
    delegate = ClipboardItemDelegate(display_lines=17)
    small_text = _height(delegate, text_index)
    small_image = _height(delegate, image_index)

    delegate.set_image_size("large")

    assert _height(delegate, text_index) == small_text
    assert _height(delegate, image_index) > small_image


def test_unknown_size_falls_back_to_one_row(make_index):
    image_index = make_index(_item("image", _data_url(64, 32)))

    assert _height(ClipboardItemDelegate(image_size="huge"), image_index) == _height(
        ClipboardItemDelegate(image_size="small"), image_index
    )


def test_wide_thumbnail_keeps_its_aspect_ratio(make_index):
    item = _item("image", _data_url(64, 16))
    index = make_index(item)
    delegate = ClipboardItemDelegate(
        theme=get_theme_manager().get_current_theme(), image_size="large", show_shortcuts=False
    )
    height = _height(delegate, index)
    canvas = QImage(300, height, QImage.Format.Format_ARGB32)
    canvas.fill(QColor("white"))
    option = QStyleOptionViewItem()
    option.rect = QRect(0, 0, 300, height)

    painter = QPainter(canvas)
    delegate.paint(painter, option, index)
    painter.end()

    side = height - 2
    red_rows = [
        y for y in range(height)
        if canvas.pixelColor(12 + side // 2, y).red() > 200 and canvas.pixelColor(12 + side // 2, y).green() < 60
    ]
    assert red_rows, "thumbnail was not drawn"
    assert abs(len(red_rows) - side // 4) <= 2


class TestSetting:
    def test_defaults_to_small(self, tmp_settings):
        assert ToolSettingsManager(qsettings=tmp_settings).get_clipboard_image_size() == "small"

    def test_round_trips(self, tmp_settings):
        manager = ToolSettingsManager(qsettings=tmp_settings)

        manager.set_clipboard_image_size("large")

        assert manager.get_clipboard_image_size() == "large"

    def test_rejects_unknown_values(self, tmp_settings):
        manager = ToolSettingsManager(qsettings=tmp_settings)
        manager.set_clipboard_image_size("medium")

        manager.set_clipboard_image_size("huge")

        assert manager.get_clipboard_image_size() == "medium"

    def test_stored_garbage_reads_as_default(self, tmp_settings):
        tmp_settings.setValue("clipboard/image_size", "huge")

        assert ToolSettingsManager(qsettings=tmp_settings).get_clipboard_image_size() == "small"
