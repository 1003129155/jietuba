# -*- coding: utf-8 -*-
"""管理窗口里的可复用控件：可排序列表的拖放与绘制、图片表单控件、图片条目操作。"""

from datetime import datetime
from types import SimpleNamespace

import pytest
from PySide6.QtCore import QEvent, QMimeData, QPoint, QPointF, Qt, QUrl
from PySide6.QtGui import (
    QColor,
    QDragEnterEvent,
    QDragLeaveEvent,
    QDragMoveEvent,
    QDropEvent,
    QEnterEvent,
    QImage,
    QMouseEvent,
)
from PySide6.QtWidgets import QListWidget, QListWidgetItem

import clipboard.ui.image_item_actions as image_item_actions
import clipboard.ui.widgets.reorder_list as reorder_list_mod
from clipboard.core import ClipboardItem
from clipboard.ui.forms.form_widgets import OptionCard
from clipboard.ui.forms.image_content_form import (
    ImageDropZone,
    ImagePreview,
    _format_bytes,
    image_from_mime,
)
from clipboard.ui.widgets.reorder_list import ID_ROLE, ReorderListWidget


@pytest.fixture
def track():
    widgets = []
    yield widgets.append
    for widget in reversed(widgets):
        widget.close()
        widget.deleteLater()


def _fill(widget, ids):
    for item_id in ids:
        row = QListWidgetItem(str(item_id))
        row.setData(ID_ROLE, item_id)
        widget.addItem(row)


def _shown_list(qapp, track, ids, height=300):
    widget = ReorderListWidget()
    track(widget)
    _fill(widget, ids)
    widget.resize(200, height)
    widget.show()
    qapp.processEvents()
    return widget


def _ids(widget):
    return [widget.item(row).data(ID_ROLE) for row in range(widget.count())]


def _lower_half(widget, row) -> QPoint:
    return widget.visualItemRect(widget.item(row)).center() + QPoint(0, 1)


def _with_source(event_cls, source, *args):
    """构造的拖放事件没有真实的 QDrag，source() 恒为 None；这里指定拖动来源。"""

    class _Event(event_cls):
        def source(self):
            return source

    return _Event(*args)


_MOVE = Qt.DropAction.MoveAction
_LEFT = Qt.MouseButton.LeftButton
_NO_MOD = Qt.KeyboardModifier.NoModifier


def _enter(source, pos, mime):
    return _with_source(QDragEnterEvent, source, pos, _MOVE, mime, _LEFT, _NO_MOD)


def _move(source, pos, mime):
    return _with_source(QDragMoveEvent, source, pos, _MOVE, mime, _LEFT, _NO_MOD)


def _drop(source, pos, mime):
    return _with_source(QDropEvent, source, QPointF(pos), _MOVE, mime, _LEFT, _NO_MOD)


class TestReorderDragAndDrop:
    def test_dropping_on_the_lower_half_inserts_below_that_row(self, qapp, track):
        widget = _shown_list(qapp, track, [1, 2, 3, 4])
        moves = []
        widget.item_moved.connect(lambda *args: moves.append(args))
        widget.setCurrentRow(0)
        mime = QMimeData()
        pos = _lower_half(widget, 2)

        enter = _enter(widget, pos, mime)
        widget.dragEnterEvent(enter)
        move = _move(widget, pos, mime)
        widget.dragMoveEvent(move)
        assert enter.isAccepted() and move.isAccepted()
        assert widget._drop_row == 3

        widget.dropEvent(_drop(widget, pos, mime))

        assert _ids(widget) == [2, 3, 1, 4]
        assert moves == [(1, 3, 4)]
        assert widget._drop_row is None

    def test_drop_position_outside_the_rows(self, qapp, track):
        widget = _shown_list(qapp, track, [1, 2, 3])

        assert widget._drop_row_at(QPoint(5, widget.viewport().height() - 1)) == 3
        assert widget._drop_row_at(QPoint(5, -5)) == 0
        top = widget.visualItemRect(widget.item(1)).top() + 1
        assert widget._drop_row_at(QPoint(5, top)) == 1

    def test_internal_drag_is_refused_while_reordering_is_off(self, qapp, track):
        widget = _shown_list(qapp, track, [1, 2, 3])
        widget.set_reorder_enabled(False)
        mime = QMimeData()
        pos = _lower_half(widget, 0)

        enter = _enter(widget, pos, mime)
        widget.dragEnterEvent(enter)
        move = _move(widget, pos, mime)
        widget.dragMoveEvent(move)
        drop = _drop(widget, pos, mime)
        widget.dropEvent(drop)

        assert not enter.isAccepted() and not move.isAccepted() and not drop.isAccepted()
        assert _ids(widget) == [1, 2, 3]

    def test_drag_from_an_unknown_source_is_refused(self, qapp, track):
        widget = _shown_list(qapp, track, [1, 2])
        enter = _enter(None, _lower_half(widget, 0), QMimeData())

        widget.dragEnterEvent(enter)

        assert not enter.isAccepted()

    def test_leaving_clears_the_drop_indicator(self, qapp, track):
        widget = _shown_list(qapp, track, [1, 2])
        widget._drop_row = 1
        widget._drop_target_row = 0

        widget.dragLeaveEvent(QDragLeaveEvent())

        assert widget._drop_row is None and widget._drop_target_row is None


class TestForeignDrop:
    @pytest.fixture
    def lists(self, qapp, track):
        source = QListWidget()
        track(source)
        _fill(source, [10])
        source.setCurrentRow(0)
        groups = _shown_list(qapp, track, [1, 2, 3])
        groups.accept_items_from(source, lambda dragged, target: target != 2)
        dropped = []
        groups.item_dropped_on.connect(lambda *args: dropped.append(args))
        return source, groups, dropped

    def test_item_from_the_other_list_is_dropped_on_an_allowed_row(self, lists):
        source, groups, dropped = lists
        mime = QMimeData()
        pos = _lower_half(groups, 2)

        enter = _enter(source, pos, mime)
        groups.dragEnterEvent(enter)
        assert enter.isAccepted() and groups._foreign_drag_inside is True

        move = _move(source, pos, mime)
        groups.dragMoveEvent(move)
        assert move.isAccepted() and groups._drop_target_row == 2

        groups.dropEvent(_drop(source, pos, mime))

        assert dropped == [(10, 3)]
        assert groups._drop_target_row is None
        assert _ids(groups) == [1, 2, 3]

    def test_rows_rejected_by_the_filter_do_not_accept_the_drop(self, lists):
        source, groups, dropped = lists
        mime = QMimeData()
        pos = _lower_half(groups, 1)

        move = _move(source, pos, mime)
        groups.dragMoveEvent(move)
        drop = _drop(source, pos, mime)
        groups.dropEvent(drop)

        assert not move.isAccepted() and groups._drop_target_row is None
        assert not drop.isAccepted()
        assert dropped == []

    def test_droppable_rows_follow_the_filter_only_while_awaiting(self, lists):
        source, groups, _dropped = lists
        assert groups.is_row_droppable(1) is True

        groups.set_awaiting_foreign_drop(True, "hint")
        assert [groups.is_row_droppable(row) for row in range(3)] == [True, False, True]
        assert groups.is_row_droppable(99) is False

        source.setCurrentRow(-1)
        assert groups.is_row_droppable(0) is False


class TestReorderPainting:
    def test_every_drag_feedback_state_renders(self, qapp, track):
        widget = _shown_list(qapp, track, [1, 2, 3])

        for state in (
            {"_drop_row": 1},
            {"_drop_row": 3},
            {"_drop_row": None, "_drop_target_row": 1},
        ):
            for name, value in state.items():
                setattr(widget, name, value)
            assert not widget.grab().isNull()

        widget._drop_target_row = None
        widget.set_awaiting_foreign_drop(True, "拖到分组上即可移动")
        assert not widget.grab().isNull()
        widget.set_awaiting_foreign_drop(True, "")
        assert not widget.grab().isNull()
        widget._foreign_drag_inside = True
        assert not widget.grab().isNull()

    def test_empty_list_draws_no_insert_line(self, qapp, track):
        widget = _shown_list(qapp, track, [])
        widget._drop_row = 0

        assert not widget.grab().isNull()

    def test_drag_pixmap_is_a_translucent_copy_of_the_row(self, qapp, track):
        widget = _shown_list(qapp, track, [1])
        rect = widget.visualItemRect(widget.item(0))

        pixmap = widget._drag_pixmap(rect)

        assert pixmap.size() == widget.viewport().grab(rect).size()


class _FakeDrag:
    instances = []

    def __init__(self, source):
        self.source = source
        self.executed = None
        _FakeDrag.instances.append(self)

    def setMimeData(self, mime):
        self.mime = mime

    def setPixmap(self, pixmap):
        self.pixmap = pixmap

    def setHotSpot(self, point):
        self.hot_spot = point

    def exec(self, action):
        self.executed = action
        self.source._drop_row = 1
        return action


class TestStartDrag:
    def test_start_drag_announces_start_and_finish(self, qapp, track, monkeypatch):
        monkeypatch.setattr(reorder_list_mod, "QDrag", _FakeDrag)
        _FakeDrag.instances.clear()
        widget = _shown_list(qapp, track, [1, 2])
        events = []
        widget.drag_started.connect(lambda: events.append("start"))
        widget.drag_finished.connect(lambda: events.append("finish"))
        widget.setCurrentRow(1)

        widget.startDrag(_MOVE)

        assert events == ["start", "finish"]
        assert _FakeDrag.instances[-1].executed == _MOVE
        assert widget._drop_row is None

    def test_start_drag_without_a_current_row_does_nothing(self, qapp, track, monkeypatch):
        monkeypatch.setattr(reorder_list_mod, "QDrag", _FakeDrag)
        _FakeDrag.instances.clear()
        widget = _shown_list(qapp, track, [1])
        widget.setCurrentRow(-1)

        widget.startDrag(_MOVE)

        assert _FakeDrag.instances == []


def test_auto_scroll_tick_scrolls_until_the_end(qapp, track):
    widget = _shown_list(qapp, track, list(range(100)), height=150)
    bar = widget.verticalScrollBar()
    widget._scroll_step = 10

    widget._auto_scroll_tick()
    assert bar.value() == 10
    assert widget._drop_row is not None

    bar.setValue(bar.maximum())
    widget._scroll_timer.start()
    widget._auto_scroll_tick()
    assert not widget._scroll_timer.isActive()


def _image(width=40, height=30, fmt=QImage.Format.Format_ARGB32):
    image = QImage(width, height, fmt)
    image.fill(QColor(80, 120, 160, 128))
    return image


class TestImagePreview:
    @pytest.mark.parametrize("fmt", [QImage.Format.Format_ARGB32, QImage.Format.Format_RGB32])
    def test_renders_with_and_without_transparency(self, qapp, track, fmt):
        preview = ImagePreview(_image(600, 400, fmt))
        track(preview)
        preview.resize(300, 10)

        assert not preview.grab().isNull()
        assert not preview.grab().isNull()      # 第二次走缓存的缩放图
        assert preview.height() == preview._box_height(300)
        assert preview.rect().contains(preview.image_rect())
        assert preview.sizeHint().height() == preview._box_height(preview.width())

    def test_small_images_are_not_enlarged(self, qapp, track):
        preview = ImagePreview(_image(20, 10))
        track(preview)
        preview.resize(300, 200)

        rect = preview.image_rect()

        dpr = preview.devicePixelRatioF()
        assert (rect.width(), rect.height()) == (round(20 / dpr), round(10 / dpr))

    def test_only_left_double_click_pins(self, qapp, track):
        preview = ImagePreview(_image())
        track(preview)
        clicks = []
        preview.double_clicked.connect(lambda: clicks.append(1))

        for button in (Qt.MouseButton.RightButton, Qt.MouseButton.LeftButton):
            event = QMouseEvent(QEvent.Type.MouseButtonDblClick, QPointF(5, 5), QPointF(5, 5),
                                button, button, _NO_MOD)
            preview.mouseDoubleClickEvent(event)

        assert clicks == [1]


def test_format_bytes_switches_to_megabytes():
    assert _format_bytes(100) == "1 KB"
    assert _format_bytes(3 * 1024 * 1024) == "3.0 MB"


class TestImageFromMime:
    def test_prefers_the_image_data(self, qapp):
        mime = QMimeData()
        mime.setImageData(_image(8, 6))

        assert image_from_mime(mime).size().toTuple() == (8, 6)

    def test_falls_back_to_the_first_image_file(self, qapp, tmp_path):
        path = tmp_path / "shot.png"
        _image(12, 9).save(str(path))
        mime = QMimeData()
        mime.setUrls([QUrl.fromLocalFile(str(tmp_path / "notes.txt")), QUrl.fromLocalFile(str(path))])

        assert image_from_mime(mime).size().toTuple() == (12, 9)

    def test_returns_none_without_an_image(self, qapp):
        mime = QMimeData()
        mime.setText("hello")

        assert image_from_mime(mime) is None


class TestImageDropZone:
    @pytest.fixture
    def zone(self, qapp, track):
        zone = ImageDropZone("拖入图片")
        track(zone)
        zone.resize(300, 160)
        dropped = []
        zone.image_dropped.connect(dropped.append)
        return zone, dropped

    def test_prompt_renders_idle_and_while_dragging(self, zone):
        widget, _dropped = zone
        assert not widget.grab().isNull()
        widget._set_drag_active(True)
        assert not widget.grab().isNull()

    def test_dropping_an_image_emits_it(self, zone):
        widget, dropped = zone
        mime = QMimeData()
        mime.setImageData(_image(8, 6))

        enter = QDragEnterEvent(QPoint(5, 5), Qt.DropAction.CopyAction, mime, _LEFT, _NO_MOD)
        widget.dragEnterEvent(enter)
        assert enter.isAccepted() and widget._drag_active is True

        widget.dragLeaveEvent(QDragLeaveEvent())
        assert widget._drag_active is False

        widget.dropEvent(QDropEvent(QPointF(5, 5), Qt.DropAction.CopyAction, mime, _LEFT, _NO_MOD))
        assert [image.size().toTuple() for image in dropped] == [(8, 6)]

    def test_non_image_drags_are_refused(self, zone):
        widget, dropped = zone
        mime = QMimeData()
        mime.setText("hello")

        enter = QDragEnterEvent(QPoint(5, 5), Qt.DropAction.CopyAction, mime, _LEFT, _NO_MOD)
        widget.dragEnterEvent(enter)
        drop = QDropEvent(QPointF(5, 5), Qt.DropAction.CopyAction, mime, _LEFT, _NO_MOD)
        widget.dropEvent(drop)

        assert not enter.isAccepted() and not drop.isAccepted()
        assert dropped == []

    def test_setting_an_image_swaps_the_prompt_for_a_preview(self, zone):
        widget, _dropped = zone
        widget.set_image(_image())
        first = widget._preview
        widget.set_image(_image(50, 20))

        assert widget._preview is not None and widget._preview is not first
        assert not widget.grab().isNull()

        widget.set_image(None)
        assert widget._preview is None


class TestOptionCard:
    def test_renders_checked_and_hovered_states(self, qapp, track):
        card = OptionCard("通用", "粘贴文字或文件")
        track(card)
        card.resize(card.sizeHint())

        assert not card.grab().isNull()
        card.setChecked(True)
        assert not card.grab().isNull()
        card.enterEvent(QEnterEvent(QPointF(1, 1), QPointF(1, 1), QPointF(1, 1)))
        card.leaveEvent(QEvent(QEvent.Type.Leave))
        assert card.minimumSizeHint().width() < card.sizeHint().width()


class _ImageManager:
    def __init__(self, data):
        self.data = data

    def get_image_data(self, image_id):
        return self.data


def _png_bytes():
    from PySide6.QtCore import QBuffer, QByteArray

    data = QByteArray()
    buffer = QBuffer(data)
    buffer.open(QBuffer.OpenModeFlag.WriteOnly)
    _image(4, 3).save(buffer, "PNG")
    return bytes(data)


def _image_item(created_at=None):
    return ClipboardItem(id=7, content="[4x3]", content_type="image", image_id="abc", created_at=created_at)


class TestImageItemActions:
    @pytest.fixture
    def warnings(self, monkeypatch):
        seen = []
        monkeypatch.setattr(image_item_actions, "show_warning_dialog", lambda _parent, _title, text: seen.append(text))
        return seen

    @pytest.fixture
    def save_dialog(self, monkeypatch):
        state = {"answer": ("", ""), "default_name": None}

        def get_save_file_name(_parent, _caption, default_name, _filters):
            state["default_name"] = default_name
            return state["answer"]

        monkeypatch.setattr(image_item_actions, "QFileDialog", SimpleNamespace(getSaveFileName=get_save_file_name))
        return state

    @pytest.fixture
    def saves(self, monkeypatch):
        state = {"ok": True, "calls": []}

        class _SaveService:
            def save_qimage_to_path(self, image, path, image_format):
                state["calls"].append((path, image_format))
                return state["ok"]

        monkeypatch.setattr("core.save.SaveService", _SaveService)
        return state

    def test_load_returns_none_for_anything_but_a_decodable_image(self, qapp):
        text_item = ClipboardItem(id=1, content="x", content_type="text")

        assert image_item_actions.load_item_image(_ImageManager(b""), None) is None
        assert image_item_actions.load_item_image(_ImageManager(b""), text_item) is None
        assert image_item_actions.load_item_image(_ImageManager(b""), _image_item()) is None
        assert image_item_actions.load_item_image(_ImageManager(b"not a png"), _image_item()) is None
        assert image_item_actions.load_item_image(_ImageManager(_png_bytes()), _image_item()).width() == 4

    def test_missing_image_only_warns(self, qapp, warnings, save_dialog):
        image_item_actions.save_image_item_as(None, _ImageManager(None), _image_item())
        image_item_actions.save_image_item_as(None, _ImageManager(None), None)

        assert warnings == [image_item_actions._tr("Image data is unavailable.")]
        assert save_dialog["default_name"] is None

    def test_cancelling_the_dialog_saves_nothing(self, qapp, warnings, save_dialog, saves):
        image_item_actions.save_image_item_as(None, _ImageManager(_png_bytes()), _image_item())

        assert save_dialog["default_name"] == "clipboard_image_7.png"
        assert saves["calls"] == [] and warnings == []

    def test_missing_extension_comes_from_the_chosen_filter(self, qapp, warnings, save_dialog, saves):
        save_dialog["answer"] = ("C:/out/shot", "JPEG Image (*.jpg *.jpeg)")
        item = _image_item(created_at=datetime(2026, 9, 24, 8, 30, 5))

        image_item_actions.save_image_item_as(None, _ImageManager(_png_bytes()), item)

        assert save_dialog["default_name"] == "clipboard_image_20260924_083005.png"
        assert saves["calls"] == [("C:/out/shot.jpg", "JPG")]
        assert warnings == []

    def test_failed_save_warns(self, qapp, warnings, save_dialog, saves):
        save_dialog["answer"] = ("C:/out/shot.webp", "")
        saves["ok"] = False

        image_item_actions.save_image_item_as(None, _ImageManager(_png_bytes()), _image_item())

        assert saves["calls"] == [("C:/out/shot.webp", "WEBP")]
        assert warnings == [image_item_actions._tr("Failed to save image.")]

    def test_format_falls_back_to_png(self):
        assert image_item_actions.format_from_save_filter("a", "Bitmap Image (*.bmp)") == "BMP"
        assert image_item_actions.format_from_save_filter("a", "All files") == "PNG"
