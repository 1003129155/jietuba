# -*- coding: utf-8 -*-
"""剪贴板管理窗口的排序与列表行为。"""

import json
from datetime import datetime

import pytest
from PySide6.QtCore import QEvent, QObject, Qt
from PySide6.QtGui import QKeyEvent
from PySide6.QtWidgets import QApplication, QListWidgetItem, QWidget

import clipboard.ui.dialogs.manage_dialog as manage_dialog_mod
from clipboard.core import ClipboardItem
from clipboard.ui.dialogs.manage_dialog import ManageDialog
from clipboard.ui.widgets.manage_rows import ICON_ROLE, SEARCH_ROLE
from clipboard.ui.widgets.reorder_list import ID_ROLE, ReorderListWidget
from core.ui_scale import dialog_scaled
from tests.test_clipboard_manage_dialog import DummyClipboardManager


def _list_with_ids(ids):
    widget = ReorderListWidget()
    for item_id in ids:
        row = QListWidgetItem(str(item_id))
        row.setData(ID_ROLE, item_id)
        widget.addItem(row)
    moves = []
    widget.item_moved.connect(lambda *args: moves.append(args))
    return widget, moves


def _ids(widget):
    return [widget.item(row).data(ID_ROLE) for row in range(widget.count())]


def _alt_key(key):
    return QKeyEvent(QKeyEvent.Type.KeyPress, key, Qt.KeyboardModifier.AltModifier)


class TestReorderListWidget:
    def test_move_row_reports_the_new_neighbours(self, qapp):
        widget, moves = _list_with_ids([1, 2, 3, 4])

        assert widget.move_row(0, 3) is True

        assert _ids(widget) == [2, 3, 1, 4]
        assert moves == [(1, 3, 4)]
        assert widget.currentRow() == 2

    def test_moving_does_not_pass_through_the_neighbour_as_current(self, qapp):
        widget, _moves = _list_with_ids([1, 2, 3])
        widget.setCurrentRow(2)
        seen = []
        widget.currentItemChanged.connect(lambda current, _prev: seen.append(current.data(ID_ROLE)))

        widget.move_row(2, 1)

        assert seen == []
        assert widget.currentItem().data(ID_ROLE) == 3

    def test_moving_onto_its_own_position_is_a_no_op(self, qapp):
        widget, moves = _list_with_ids([1, 2, 3])

        assert widget.move_row(1, 1) is False
        assert widget.move_row(1, 2) is False
        assert moves == []

    def test_top_and_bottom_leave_one_side_without_a_neighbour(self, qapp):
        widget, moves = _list_with_ids([1, 2, 3])
        widget.setCurrentRow(1)
        widget.move_current_to_top()
        widget.setCurrentRow(1)
        widget.move_current_to_bottom()

        assert _ids(widget) == [2, 3, 1]
        assert moves == [(2, None, 1), (1, 3, None)]

    def test_alt_arrow_keys_move_the_current_row_one_step(self, qapp):
        widget, moves = _list_with_ids([1, 2, 3])
        widget.setCurrentRow(1)

        widget.keyPressEvent(_alt_key(Qt.Key.Key_Down))
        assert _ids(widget) == [1, 3, 2]
        widget.keyPressEvent(_alt_key(Qt.Key.Key_Up))
        widget.keyPressEvent(_alt_key(Qt.Key.Key_Up))
        assert _ids(widget) == [2, 1, 3]

        # 已经在最上面时再上移不产生移动
        widget.keyPressEvent(_alt_key(Qt.Key.Key_Up))
        assert len(moves) == 3

    def test_keyboard_reorder_is_ignored_while_disabled(self, qapp):
        widget, moves = _list_with_ids([1, 2, 3])
        widget.setCurrentRow(1)
        widget.set_reorder_enabled(False)

        widget.keyPressEvent(_alt_key(Qt.Key.Key_Down))

        assert _ids(widget) == [1, 2, 3]
        assert moves == []
        # 过滤时只禁止本列表内排序，拖到别的分组仍然可以
        assert widget.dragEnabled() is True

    def test_auto_scroll_speeds_up_towards_the_edges(self, qapp):
        widget, _moves = _list_with_ids(list(range(100)))
        widget.resize(300, 400)
        height = widget.viewport().height()
        margin = dialog_scaled(56)

        widget._update_auto_scroll(height // 2)
        assert widget._scroll_step == 0

        widget._update_auto_scroll(margin - 4)
        slow_up = widget._scroll_step
        widget._update_auto_scroll(0)
        fast_up = widget._scroll_step
        assert fast_up < slow_up < 0

        widget._update_auto_scroll(height - 1)
        assert widget._scroll_step > 0
        assert widget._scroll_timer.isActive()

        widget._end_drag_feedback()
        assert not widget._scroll_timer.isActive()


def _manager_with_items(count, group_id=1):
    manager = DummyClipboardManager()
    for item_id in range(1, count + 1):
        manager.items[item_id] = ClipboardItem(
            id=item_id,
            content=f"第 {item_id} 条\n备注 {item_id}",
            content_type="text",
            title="带标题" if item_id == 2 else None,
            created_at=datetime(2025, 1, 1, 9, 0),
        )
        manager.item_groups[item_id] = group_id
    return manager


@pytest.fixture
def make_dialog(qapp):
    dialogs = []

    def factory(manager):
        manage_dialog_mod._manage_window_instance = None
        dialog = ManageDialog(manager)
        dialogs.append(dialog)
        return dialog

    yield factory
    for dialog in dialogs:
        dialog.hide()
        dialog.deleteLater()
    manage_dialog_mod._manage_window_instance = None


class TestManageDialogLists:
    def test_opening_shows_the_first_group_with_all_its_items(self, make_dialog):
        dialog = make_dialog(_manager_with_items(120))

        assert dialog.selected_group_id == 1
        assert dialog.item_list.count() == 120
        assert dialog.current_mode == "content"
        assert dialog.save_btn.text() == dialog.tr("Add")

    def test_rows_are_single_line_like_the_clipboard_panel(self, make_dialog):
        manager = _manager_with_items(3)
        manager.items[4] = ClipboardItem(
            id=4, content=json.dumps({"files": [r"C:\x\Output.txt"]}), content_type="file")
        manager.item_groups[4] = 1
        dialog = make_dialog(manager)

        untitled, titled, file_row = (dialog.item_list.item(row) for row in (0, 1, 3))
        assert untitled.text() == "第 1 条 备注 1"
        assert titled.text() == "带标题"
        assert (file_row.text(), file_row.data(ICON_ROLE)) == ("Output.txt", "📁")
        assert file_row.toolTip() == ""
        assert file_row.data(SEARCH_ROLE) == r"C:\x\Output.txt"

    def test_selecting_another_group_reloads_the_middle_column(self, make_dialog):
        manager = _manager_with_items(3)
        manager.items[9] = ClipboardItem(id=9, content="学习笔记", content_type="text")
        manager.item_groups[9] = 2
        dialog = make_dialog(manager)

        dialog.group_list.setCurrentRow(1)

        assert dialog.selected_group_id == 2
        assert _ids(dialog.item_list) == [9]

    def test_dragged_item_is_saved_between_its_new_neighbours(self, make_dialog):
        manager = _manager_with_items(5)
        dialog = make_dialog(manager)

        dialog.item_list.move_row(4, 1)

        assert manager.item_moves == [(5, 1, 2)]
        assert _ids(dialog.item_list) == [1, 5, 2, 3, 4]

    def test_group_reorder_is_saved_and_announced(self, make_dialog):
        manager = _manager_with_items(1)
        dialog = make_dialog(manager)
        announced = []
        dialog.group_added.connect(lambda: announced.append("group"))

        dialog.group_list.setCurrentRow(1)
        dialog.group_list.move_current_to_top()

        assert manager.group_moves == [(2, None, 1)]
        assert announced == ["group"]

    def test_search_filters_rows_and_pauses_reordering(self, make_dialog):
        dialog = make_dialog(_manager_with_items(12))

        dialog.search_input.setText("第 1")

        visible = [
            dialog.item_list.item(row).data(ID_ROLE)
            for row in range(dialog.item_list.count())
            if not dialog.item_list.isRowHidden(row)
        ]
        assert visible == [1, 10, 11, 12]
        assert dialog.item_list.reorder_enabled() is False
        assert dialog.list_count.text() == dialog.tr("{visible} of {count} items").format(visible=4, count=12)

        dialog.search_input.clear()
        assert dialog.item_list.reorder_enabled() is True

    def test_empty_group_shows_a_hint_instead_of_the_list(self, make_dialog):
        dialog = make_dialog(DummyClipboardManager())

        assert dialog.list_stack.currentWidget() is dialog.empty_label
        assert dialog.empty_label.text() == dialog.tr("No content in this group yet")

    def test_new_group_becomes_the_selected_group(self, make_dialog):
        manager = _manager_with_items(1)
        dialog = make_dialog(manager)

        dialog._show_new_group_form()
        dialog.group_name_input.setText("新分组")
        dialog._save_group()

        new_id = manager.created_groups[-1].id
        assert dialog.selected_group_id == new_id
        assert dialog.group_list.currentItem().data(ID_ROLE) == new_id

    def test_open_item_editor_selects_the_row(self, make_dialog):
        dialog = make_dialog(_manager_with_items(80))

        dialog.open_item_editor(64, 1)

        assert dialog.item_list.currentItem().data(ID_ROLE) == 64
        assert dialog.content_edit.toPlainText() == "第 64 条\n备注 64"

    def test_enter_in_the_search_box_does_not_press_a_button(self, make_dialog):
        dialog = make_dialog(_manager_with_items(1))

        assert all(not button.autoDefault() for button in (dialog.save_btn, dialog.delete_btn, dialog.new_group_btn))


class _TopLevelShowSpy(QObject):
    def __init__(self, ignore):
        super().__init__()
        self._ignore = ignore
        self.shown = []

    def eventFilter(self, obj, event):
        if (event.type() == QEvent.Type.Show and isinstance(obj, QWidget)
                and obj.isWindow() and obj is not self._ignore):
            self.shown.append(type(obj).__name__)
        return False


def test_rebuilding_the_form_twice_in_a_row_never_flashes_a_window(qapp, make_dialog):
    """上一份表单的控件还没来得及显示就被摘掉时，不能变成独立窗口弹出来。"""
    dialog = make_dialog(_manager_with_items(3))
    dialog.show()
    qapp.processEvents()
    spy = _TopLevelShowSpy(dialog)
    QApplication.instance().installEventFilter(spy)
    try:
        dialog._show_edit_content_form(1)
        dialog._show_edit_content_form(2)
        qapp.processEvents()
    finally:
        QApplication.instance().removeEventFilter(spy)

    assert spy.shown == []


def _png_bytes(width=30, height=20):
    from PySide6.QtCore import QBuffer, QByteArray
    from PySide6.QtGui import QColor, QImage

    image = QImage(width, height, QImage.Format.Format_ARGB32)
    image.fill(QColor("#6F8FAB"))
    data = QByteArray()
    buffer = QBuffer(data)
    buffer.open(QBuffer.OpenModeFlag.WriteOnly)
    image.save(buffer, "PNG")
    return bytes(data)


def _manager_with_image(image_data):
    manager = _manager_with_items(1)
    manager.items[7] = ClipboardItem(
        id=7, content="[30x20]", content_type="image", image_id="abc", source_app="msedge.exe")
    manager.item_groups[7] = 1
    manager.get_image_data = lambda image_id: image_data if image_id == "abc" else None
    return manager


class TestNewContentKind:
    def test_general_group_offers_text_and_file(self, make_dialog):
        dialog = make_dialog(_manager_with_items(1))

        assert dialog.content_kind_switch.currentItem() == "text"
        assert hasattr(dialog, "content_edit")

    def test_switching_to_file_keeps_the_typed_title(self, make_dialog):
        dialog = make_dialog(_manager_with_items(1))
        dialog.title_input.setText("常用文档")

        dialog.content_kind_switch.setCurrentItem("file")

        assert dialog.file_path_input.isReadOnly() is True
        assert dialog.title_input.text() == "常用文档"

    def test_file_added_to_a_general_group_is_stored_as_file(self, make_dialog):
        manager = _manager_with_items(1)
        dialog = make_dialog(manager)
        dialog.content_kind_switch.setCurrentItem("file")
        dialog._set_selected_file_path(r"C:\docs\report.pdf")

        dialog._save_content()

        item_id, content, content_type, _title = manager.added_items[-1]
        assert content_type == "file"
        assert "report.pdf" in content
        assert manager.move_requests[-1] == (item_id, 1)

    def test_quick_launch_group_has_no_switch(self, make_dialog):
        from clipboard.core import Group, GroupType

        manager = _manager_with_items(1)
        manager.groups.append(Group(id=3, name="快速启动", icon="⚡", group_type=GroupType.FILE))
        dialog = make_dialog(manager)

        dialog.group_list.setCurrentRow(2)

        assert dialog.content_kind_switch is None
        assert dialog.file_path_input.isReadOnly() is True


class TestImageItemForm:
    def test_image_item_shows_a_preview_instead_of_a_text_box(self, make_dialog):
        dialog = make_dialog(_manager_with_image(_png_bytes()))

        dialog._show_edit_content_form(7)

        assert dialog.image_preview is not None
        widgets = [dialog.detail_layout.itemAt(i).widget() for i in range(dialog.detail_layout.count())]
        from PySide6.QtWidgets import QTextEdit
        assert not any(isinstance(widget, QTextEdit) for widget in widgets)

    def test_saving_an_image_item_only_changes_the_title(self, make_dialog):
        manager = _manager_with_image(_png_bytes())
        dialog = make_dialog(manager)
        dialog._show_edit_content_form(7)
        dialog.title_input.setText("产品图")

        dialog._save_content()

        assert manager.updated_items[-1] == (7, "[30x20]", "产品图")

    def test_saving_repairs_content_damaged_by_the_old_text_editor(self, make_dialog):
        manager = _manager_with_image(_png_bytes(30, 20))
        manager.items[7].content = "[30x20]1212"
        dialog = make_dialog(manager)
        dialog._show_edit_content_form(7)

        dialog._save_content()

        assert manager.updated_items[-1] == (7, "[30x20]", None)

    def test_missing_original_disables_the_image_actions(self, make_dialog):
        from ui.fluent_lite import PushButton

        dialog = make_dialog(_manager_with_image(None))

        dialog._show_edit_content_form(7)

        assert dialog.image_preview is None
        buttons = dialog.detail_content.findChildren(PushButton)
        assert buttons and not any(button.isEnabled() for button in buttons)

    def test_double_clicking_the_preview_pins_the_image(self, make_dialog, monkeypatch):
        import clipboard.ui.windows.pin_window as pin_window_mod

        pinned = []
        monkeypatch.setattr(pin_window_mod, "create_pin_from_clipboard_item",
                            lambda item_id, manager, window: pinned.append(item_id))
        dialog = make_dialog(_manager_with_image(_png_bytes()))
        dialog._show_edit_content_form(7)

        dialog.image_preview.double_clicked.emit()

        assert pinned == [7]


class _MonitoredManager(DummyClipboardManager):
    """带一个假剪贴板监听：写入图片时像 Rust 端那样新建记录，或按内容去重把旧记录顶上来。"""

    def __init__(self, monitoring=True):
        super().__init__()
        self.monitoring = monitoring
        self.image_keys = {}

    def is_monitoring(self):
        return self.monitoring

    def capture(self, image):
        from datetime import datetime as _dt

        key = bytes(image.constBits())[:64] + str(image.size()).encode()
        item_id = self.image_keys.get(key)
        if item_id is None:
            item_id = max(self.items, default=0) + 100
            self.image_keys[key] = item_id
            self.items[item_id] = ClipboardItem(
                id=item_id, content=f"[{image.width()}x{image.height()}]", content_type="image",
                image_id=f"img{item_id}", created_at=_dt.now())
        self.items[item_id].updated_at = _dt.now()
        return item_id

    def get_history(self, offset=0, limit=50, content_type=None, **_kwargs):
        items = [item for item in self.items.values() if content_type in (None, item.content_type)]
        items.sort(key=lambda item: item.updated_at or item.created_at, reverse=True)
        return items[offset:offset + limit]

    def move_to_group(self, item_id, group_id):
        self.item_groups[item_id] = group_id
        return super().move_to_group(item_id, group_id)


@pytest.fixture
def fake_clipboard_write(monkeypatch):
    import core.clipboard_utils as clipboard_utils

    state = {"manager": None, "writes": 0}

    def write(image, file_reference=None):
        state["writes"] += 1
        QApplication.clipboard().setImage(image)
        if state["manager"] is not None and state["manager"].monitoring:
            state["manager"].capture(image)

    monkeypatch.setattr(clipboard_utils, "copy_image_to_clipboard", write)
    return state


def _open_image_form(make_dialog, manager):
    dialog = make_dialog(manager)
    dialog.content_kind_switch.setCurrentItem("image")
    return dialog


def _solid_image(color="#6F8FAB", width=40, height=30):
    from PySide6.QtGui import QColor, QImage

    image = QImage(width, height, QImage.Format.Format_ARGB32)
    image.fill(QColor(color))
    return image


class TestAddImage:
    def test_new_image_is_captured_titled_and_moved_into_the_group(self, make_dialog, fake_clipboard_write):
        manager = _MonitoredManager()
        fake_clipboard_write["manager"] = manager
        dialog = _open_image_form(make_dialog, manager)
        dialog._set_pending_image(_solid_image())
        dialog.title_input.setText("配色")

        dialog._save_content()
        dialog._poll_captured_image()

        item_id = next(i for i, item in manager.items.items() if item.content_type == "image")
        assert manager.item_groups[item_id] == 1
        assert manager.updated_items[-1] == (item_id, "[40x30]", "配色")
        assert dialog.item_list.row_of_id(item_id) >= 0
        assert dialog._pending_image is None
        assert dialog.save_btn.isEnabled()

    def test_image_already_in_another_group_asks_before_moving(self, make_dialog, fake_clipboard_write, monkeypatch):
        manager = _MonitoredManager()
        fake_clipboard_write["manager"] = manager
        existing = manager.capture(_solid_image())
        manager.item_groups[existing] = 2
        asked = []
        monkeypatch.setattr(manage_dialog_mod, "show_confirm_dialog",
                            lambda *args: asked.append(args[2]) or False)
        dialog = _open_image_form(make_dialog, manager)
        dialog._set_pending_image(_solid_image())

        dialog._save_content()
        dialog._poll_captured_image()

        assert asked and "学习" in asked[0] and "工作" in asked[0]
        assert manager.item_groups[existing] == 2

        monkeypatch.setattr(manage_dialog_mod, "show_confirm_dialog", lambda *args: True)
        dialog._save_content()
        dialog._poll_captured_image()
        assert manager.item_groups[existing] == 1

    def test_reports_a_timeout_when_nothing_is_recorded(self, make_dialog, fake_clipboard_write, monkeypatch):
        manager = _MonitoredManager()
        fake_clipboard_write["manager"] = None
        warnings = []
        monkeypatch.setattr(manage_dialog_mod, "show_warning_dialog", lambda *args: warnings.append(args[2]))
        monkeypatch.setattr(manage_dialog_mod, "_IMAGE_CAPTURE_TIMEOUT_S", 0)
        dialog = _open_image_form(make_dialog, manager)
        dialog._set_pending_image(_solid_image())

        dialog._save_content()
        dialog._poll_captured_image()

        assert warnings == [dialog.tr("The image was not recorded. Make sure clipboard history is turned on.")]
        assert dialog.save_btn.isEnabled()
        assert dialog._image_request is None

    def test_image_page_is_disabled_while_history_is_off(self, make_dialog):
        dialog = _open_image_form(make_dialog, _MonitoredManager(monitoring=False))

        assert dialog.image_drop_zone.isEnabled() is False
        assert dialog.paste_image_btn.isEnabled() is False

    def test_adding_without_an_image_only_warns(self, make_dialog, fake_clipboard_write, monkeypatch):
        manager = _MonitoredManager()
        fake_clipboard_write["manager"] = manager
        warnings = []
        monkeypatch.setattr(manage_dialog_mod, "show_warning_dialog", lambda *args: warnings.append(args[2]))
        dialog = _open_image_form(make_dialog, manager)
        dialog._set_pending_image(None)

        dialog._save_content()

        assert warnings == [dialog.tr("Please choose an image first")]
        assert fake_clipboard_write["writes"] == 0


def test_image_page_starts_empty_even_with_an_image_on_the_clipboard(make_dialog):
    """图片页不自动带入剪贴板里的图，要用户自己点“从剪贴板粘贴”。"""
    QApplication.clipboard().setImage(_solid_image("#E0B070"))
    dialog = _open_image_form(make_dialog, _MonitoredManager())

    assert dialog._pending_image is None

    dialog._use_clipboard_image()
    assert dialog._pending_image is not None



def _two_group_manager():
    from clipboard.core import Group, GroupType

    manager = _MonitoredManager()
    for item_id in (1, 2):
        manager.items[item_id] = ClipboardItem(id=item_id, content=f"文本 {item_id}", content_type="text")
        manager.item_groups[item_id] = 1
    manager.items[3] = ClipboardItem(id=3, content='{"files": ["C:/a.txt"]}', content_type="file")
    manager.item_groups[3] = 1
    manager.groups.append(Group(id=3, name="快速启动", icon="⚡", group_type=GroupType.FILE))
    return manager


class TestMoveItemToGroup:
    def test_drop_rules_follow_group_types(self, make_dialog):
        dialog = make_dialog(_two_group_manager())

        assert dialog._can_move_item_to_group(1, 1) is False      # 当前分组
        assert dialog._can_move_item_to_group(1, 2) is True
        assert dialog._can_move_item_to_group(1, 3) is False      # 快速启动只收文件
        assert dialog._can_move_item_to_group(3, 3) is True

    def test_moved_item_leaves_the_list_and_the_editor(self, make_dialog):
        manager = _two_group_manager()
        dialog = make_dialog(manager)
        dialog._show_edit_content_form(2)
        changed = []
        dialog.content_added.connect(changed.append)

        dialog._move_item_to_group(2, 2)

        assert manager.item_groups[2] == 2
        assert _ids(dialog.item_list) == [1, 3]
        assert dialog.editing_item_id is None
        assert dialog.save_btn.text() == dialog.tr("Add")
        assert changed == [1, 2]

    def test_disallowed_move_does_nothing(self, make_dialog):
        manager = _two_group_manager()
        dialog = make_dialog(manager)

        dialog._move_item_to_group(1, 3)

        assert manager.item_groups[1] == 1
        assert manager.move_requests == []

    def test_group_list_accepts_items_from_the_content_list(self, make_dialog):
        dialog = make_dialog(_two_group_manager())

        assert dialog.group_list._foreign_source is dialog.item_list
        assert dialog.group_list._foreign_filter(1, 2) is True


class TestDragHints:
    def test_new_group_button_is_labelled(self, make_dialog):
        dialog = make_dialog(_two_group_manager())

        assert dialog.new_group_btn.text() == "+ " + dialog.tr("New")

    def test_group_list_shows_a_drop_overlay_only_while_dragging_content(self, make_dialog):
        dialog = make_dialog(_two_group_manager())
        groups = dialog.group_list
        assert groups._awaiting_foreign_drop is False

        dialog.item_list.setCurrentRow(0)          # 拖的是一条文本
        dialog.item_list.drag_started.emit()
        assert groups._awaiting_foreign_drop is True
        assert groups._drop_hint == dialog.tr("Drop on a group to move it there")
        droppable = [groups.is_row_droppable(row) for row in range(groups.count())]
        assert droppable == [False, True, False]   # 当前分组、可放、快速启动只收文件

        dialog.item_list.drag_finished.emit()
        assert groups._awaiting_foreign_drop is False
        assert groups._foreign_drag_inside is False
        assert all(groups.is_row_droppable(row) for row in range(groups.count()))


def test_edit_group_button_is_labelled(make_dialog):
    dialog = make_dialog(_two_group_manager())

    assert dialog.edit_group_btn.text().strip() == dialog.tr("Edit")
    assert not dialog.edit_group_btn.icon().isNull()


class TestReleaseOnClose:
    @pytest.fixture(autouse=True)
    def no_trim(self, monkeypatch):
        import core.platform_utils as platform_utils

        self.trims = []
        monkeypatch.setattr(platform_utils, "request_trim_working_set",
                            lambda *args: self.trims.append(args))

    def test_closing_drops_the_preview_list_and_thumbnail_cache(self, make_dialog):
        from clipboard.ui.widgets import manage_rows

        dialog = make_dialog(_manager_with_image(_png_bytes()))
        dialog.show()
        dialog._show_edit_content_form(7)
        manage_rows._thumbnail("data:image/png;base64,AAAA")
        assert dialog.image_preview is not None

        dialog.close()

        assert dialog.isVisible() is False
        assert dialog.image_preview is None
        assert dialog.item_list.count() == 0
        assert manage_rows._thumbnail.cache_info().currsize == 0
        assert self.trims

    def test_reopening_reloads_and_picks_up_changes_made_while_hidden(self, make_dialog):
        manager = _manager_with_items(2)
        dialog = make_dialog(manager)
        dialog.show()
        dialog.close()

        manager.items[9] = ClipboardItem(id=9, content="隐藏期间新加的", content_type="text")
        manager.item_groups[9] = 1
        dialog.show_and_activate()

        assert _ids(dialog.item_list) == [1, 2, 9]

    def test_opening_a_specific_item_after_close_is_not_overwritten(self, make_dialog):
        dialog = make_dialog(_manager_with_items(3))
        dialog.show()
        dialog.close()

        dialog.open_item_editor(2, 1)

        assert dialog.editing_item_id == 2
        assert dialog.item_list.currentItem().data(ID_ROLE) == 2


    def test_escape_also_releases_memory(self, make_dialog):
        """QDialog 按 Esc 走 reject 直接隐藏，不经过 closeEvent。"""
        dialog = make_dialog(_manager_with_items(3))
        dialog.show()
        assert dialog.item_list.count() == 3

        dialog.reject()

        assert dialog.isVisible() is False
        assert dialog.item_list.count() == 0
        assert self.trims
