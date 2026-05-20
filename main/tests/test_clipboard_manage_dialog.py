# -*- coding: utf-8 -*-
"""
剪贴板管理窗口 / 控制器回归测试

覆盖最近修改过、最容易回归的几类行为：
- 分组重名拦截
- 外部删除后的管理窗口同步刷新
- 管理窗口内删除分组状态清理
- 控制器打开编辑窗口时的 UniqueConnection 去重
"""

import json
import os
from unittest.mock import MagicMock

import pytest
from PySide6.QtCore import QObject, Signal, QUrl

import clipboard.controllers.clipboard_controller as clipboard_controller_mod
import clipboard.ui.dialogs.manage_dialog as manage_dialog_mod
from clipboard.controllers.clipboard_controller import ClipboardController
from clipboard.core import ClipboardItem, Group, GroupType
from clipboard.ui.dialogs.manage_dialog import ManageDialog


class DummyClipboardManager:
    """用于 UI/控制器测试的轻量假管理器。"""

    def __init__(self):
        self.groups = [
            Group(id=1, name="工作", icon="W"),
            Group(id=2, name="学习", icon="S"),
        ]
        self.created_groups = []
        self.deleted_groups = []
        self.updated_groups = []
        self.items = {}
        self.added_items = []
        self.move_requests = []
        self.updated_items = []
        self._next_item_id = 1

    @property
    def is_available(self):
        return True

    def get_groups(self):
        return list(self.groups)

    def create_group(self, name, color=None, icon=None):
        new_id = max((group.id for group in self.groups), default=0) + 1
        group = Group(id=new_id, name=name, color=color, icon=icon)
        self.groups.append(group)
        self.created_groups.append(group)
        return new_id

    def update_group(self, group_id, name, icon=None):
        for group in self.groups:
            if group.id == group_id:
                group.name = name
                group.icon = icon
                self.updated_groups.append((group_id, name, icon))
                return True
        return False

    def delete_group(self, group_id):
        self.deleted_groups.append(group_id)
        self.groups = [group for group in self.groups if group.id != group_id]
        return True

    def get_item(self, item_id):
        return self.items.get(item_id)

    def get_by_group(self, group_id, offset=0, limit=50):
        return []

    def add_item(self, content, content_type, title=None):
        item_id = self._next_item_id
        self._next_item_id += 1
        self.added_items.append((item_id, content, content_type, title))
        return item_id

    def move_to_group(self, item_id, group_id):
        self.move_requests.append((item_id, group_id))
        return True

    def update_item(self, item_id, content, title=None):
        self.updated_items.append((item_id, content, title))
        if item_id in self.items:
            self.items[item_id].content = content
            self.items[item_id].title = title
        return True

    def delete_item(self, item_id):
        self.items.pop(item_id, None)
        return True


class DummyManageDialog(QObject):
    """用于控制器测试的伪对话框。"""

    group_added = Signal()
    data_changed = Signal()

    def __init__(self):
        super().__init__()
        self.group_editor_requests = []
        self.item_editor_requests = []
        self.refresh_requests = []

    def open_group_editor(self, group_id):
        self.group_editor_requests.append(group_id)

    def open_item_editor(self, item_id, group_id):
        self.item_editor_requests.append((item_id, group_id))

    def refresh_after_external_change(self, deleted_group_id=None):
        self.refresh_requests.append(deleted_group_id)


class CallbackReceiver(QObject):
    def __init__(self):
        super().__init__()
        self.group_added_calls = 0
        self.data_changed_calls = 0

    def on_group_added(self):
        self.group_added_calls += 1

    def on_data_changed(self):
        self.data_changed_calls += 1


@pytest.fixture
def dialog(monkeypatch, qapp):
    manager = DummyClipboardManager()
    manage_dialog_mod._manage_window_instance = None
    dlg = ManageDialog(manager)
    yield dlg, manager
    dlg.hide()
    dlg.deleteLater()
    manage_dialog_mod._manage_window_instance = None


@pytest.fixture
def controller(monkeypatch):
    monkeypatch.setattr(ClipboardController, "_load_settings", lambda self: None)
    return ClipboardController(DummyClipboardManager())


class TestManageDialog:
    def test_group_name_exists_respects_exclude_group(self, dialog):
        dlg, _manager = dialog
        assert dlg._group_name_exists("工作") is True
        assert dlg._group_name_exists("工作", exclude_group_id=1) is False
        assert dlg._group_name_exists("不存在") is False

    def test_new_group_form_defaults_to_general_group(self, dialog):
        dlg, _manager = dialog

        dlg._show_new_group_form()

        assert dlg.detail_title.text() == dlg.tr("New General Group")
        assert dlg.detail_subtitle.wordWrap() is True
        assert dlg.detail_subtitle.text() == dlg.tr(
            "General groups allow any content type; selecting an item pastes content or files to the current target"
        )
        assert dlg.radio_normal.isChecked() is True
        assert dlg.radio_file.isChecked() is False
        assert dlg.icon_input.text() == "📁"

    def test_edit_group_form_keeps_quick_launch_group_type(self, dialog):
        dlg, manager = dialog
        quick_group = Group(id=3, name="快速启动", icon="⚡", group_type=GroupType.FILE)
        manager.groups.append(quick_group)

        dlg._show_edit_group_form(quick_group.id)

        assert dlg.detail_title.text() == dlg.tr("Edit Quick Launch Group")
        assert dlg.detail_subtitle.text() == dlg.tr(
            "Quick launch groups only allow file or folder paths; selecting an item opens them directly"
        )
        assert dlg.group_name_input.text() == "快速启动"
        assert dlg.radio_file.isChecked() is True
        assert dlg.radio_normal.isChecked() is False
        assert dlg.icon_input.text() == "⚡"

    def test_group_type_toggle_updates_default_icon(self, dialog):
        dlg, _manager = dialog

        dlg._show_new_group_form()
        dlg.radio_file.setChecked(True)
        assert dlg.detail_title.text() == dlg.tr("New Quick Launch Group")
        assert dlg.detail_subtitle.text() == dlg.tr(
            "Quick launch groups only allow file or folder paths; selecting an item opens them directly"
        )
        assert dlg.icon_input.text() == "⚡"

        dlg.radio_normal.setChecked(True)
        assert dlg.detail_title.text() == dlg.tr("New General Group")
        assert dlg.detail_subtitle.text() == dlg.tr(
            "General groups allow any content type; selecting an item pastes content or files to the current target"
        )
        assert dlg.icon_input.text() == "📁"

    def test_refresh_group_list_uses_quick_launch_default_icon(self, dialog):
        dlg, manager = dialog
        manager.groups.append(Group(id=3, name="快速启动", icon=None, group_type=GroupType.FILE))

        dlg._refresh_group_list()

        texts = [dlg.list_widget.item(i).text() for i in range(dlg.list_widget.count())]
        assert "⚡ 快速启动" in texts

    def test_make_unique_group_name_appends_incremental_suffix(self, dialog):
        dlg, _manager = dialog
        used_names = {"工作", "工作 (1)", "学习"}
        assert dlg._make_unique_group_name("新建", used_names) == "新建"
        assert dlg._make_unique_group_name("工作", used_names) == "工作 (2)"

    def test_save_group_rejects_duplicate_name_before_create(self, dialog, monkeypatch):
        dlg, manager = dialog
        warnings = []
        monkeypatch.setattr(manage_dialog_mod, "show_warning_dialog", lambda *args: warnings.append(args[2]))

        dlg.group_name_input.setText("工作")
        dlg._save_group()

        assert warnings == [dlg.tr("A group with this name already exists")]
        assert manager.created_groups == []

    def test_save_group_creates_group_with_legacy_manager_signature(self, dialog):
        dlg, manager = dialog
        signals = []

        dlg.group_added.connect(lambda: signals.append("group"))
        dlg.data_changed.connect(lambda: signals.append("data"))
        dlg._show_new_group_form()
        dlg.group_name_input.setText("快速启动")
        dlg.radio_file.setChecked(True)

        dlg._save_group()

        assert manager.created_groups[-1].name == "快速启动"
        assert manager.created_groups[-1].icon == "⚡"
        assert signals == ["group", "data"]

    def test_refresh_after_external_change_clears_deleted_group_from_ui(self, dialog):
        dlg, manager = dialog
        manager.delete_group(2)
        dlg.editing_group_id = 2

        dlg.refresh_after_external_change(deleted_group_id=2)

        assert dlg.editing_group_id is None
        texts = [dlg.list_widget.item(i).text() for i in range(dlg.list_widget.count())]
        assert all("学习" not in text for text in texts)

    def test_group_delete_clears_editing_state_without_warning(self, dialog, monkeypatch):
        dlg, manager = dialog
        confirmations = []
        warnings = []
        signals = []

        monkeypatch.setattr(manage_dialog_mod, "show_confirm_dialog", lambda *args: confirmations.append((args[1], args[2])) or True)
        monkeypatch.setattr(manage_dialog_mod, "show_warning_dialog", lambda *args: warnings.append(args[2]))

        dlg.group_added.connect(lambda: signals.append("group"))
        dlg.data_changed.connect(lambda: signals.append("data"))
        dlg.editing_group_id = 1
        dlg.current_mode = "group"

        dlg._on_delete_clicked()

        assert manager.deleted_groups == [1]
        assert dlg.editing_group_id is None
        assert warnings == []
        assert signals == ["group", "data"]
        assert confirmations[0][0] == dlg.tr("Confirm Delete")

    def test_import_reuses_existing_group_when_name_matches(self, dialog, monkeypatch, tmp_path):
        dlg, manager = dialog
        csv_path = tmp_path / "import.csv"
        csv_path.write_text(
            "Group,Content,Title\n工作,第一条内容,标题A\n工作,第二条内容,标题B\n",
            encoding="utf-8",
        )

        infos = []
        warnings = []
        monkeypatch.setattr(manage_dialog_mod.QFileDialog, "getOpenFileName", lambda *args, **kwargs: (str(csv_path), "CSV Files (*.csv)"))
        monkeypatch.setattr(manage_dialog_mod, "show_info_dialog", lambda *args: infos.append((args[1], args[2])))
        monkeypatch.setattr(manage_dialog_mod, "show_warning_dialog", lambda *args: warnings.append((args[1], args[2])))

        dlg._import_from_csv()

        assert warnings == []
        assert manager.created_groups == []
        assert [request[1] for request in manager.move_requests] == [1, 1]
        assert len(manager.added_items) == 2
        assert infos

    def test_import_creates_missing_group_only_once(self, dialog, monkeypatch, tmp_path):
        dlg, manager = dialog
        csv_path = tmp_path / "import_new_group.csv"
        csv_path.write_text(
            "Group,Content,Title\n新分组,第一条内容,标题A\n新分组,第二条内容,标题B\n",
            encoding="utf-8",
        )

        monkeypatch.setattr(manage_dialog_mod.QFileDialog, "getOpenFileName", lambda *args, **kwargs: (str(csv_path), "CSV Files (*.csv)"))
        monkeypatch.setattr(manage_dialog_mod, "show_info_dialog", lambda *args: None)
        monkeypatch.setattr(manage_dialog_mod, "show_warning_dialog", lambda *args: pytest.fail("不应触发警告"))

        dlg._import_from_csv()

        assert [group.name for group in manager.created_groups] == ["新分组"]
        new_group_id = manager.created_groups[0].id
        assert [request[1] for request in manager.move_requests] == [new_group_id, new_group_id]

    def test_open_item_editor_keeps_file_group_item_in_file_form(self, dialog):
        dlg, manager = dialog
        file_group = Group(id=3, name="快速启动", icon="⚡", group_type=GroupType.FILE)
        manager.groups.append(file_group)
        file_path = os.path.normpath(r"C:\Temp\demo.txt")
        manager.items[7] = ClipboardItem(
            id=7,
            content=json.dumps({"files": [file_path]}, ensure_ascii=False),
            content_type="file",
            title="演示文件",
        )

        dlg.open_item_editor(7, file_group.id)

        widgets = [
            dlg.detail_layout.itemAt(i).widget()
            for i in range(dlg.detail_layout.count())
            if dlg.detail_layout.itemAt(i).widget() is not None
        ]
        assert dlg.file_path_input.text() == file_path
        assert dlg.title_input.text() == "演示文件"
        assert all(not isinstance(widget, manage_dialog_mod.QTextEdit) for widget in widgets)

    def test_new_file_content_form_keeps_path_input_read_only(self, dialog):
        dlg, manager = dialog
        file_group = Group(id=3, name="快速启动", icon="⚡", group_type=GroupType.FILE)
        manager.groups.append(file_group)
        dlg.selected_group_id = file_group.id

        dlg._show_new_content_form()

        assert dlg.file_path_input.isReadOnly() is True

    def test_file_drop_zone_uses_first_dropped_path(self, dialog):
        dlg, manager = dialog
        file_group = Group(id=3, name="快速启动", icon="⚡", group_type=GroupType.FILE)
        manager.groups.append(file_group)
        dlg.selected_group_id = file_group.id
        first_path = os.path.normpath(r"C:\Temp\first.txt")
        second_path = os.path.normpath(r"C:\Temp\second.txt")

        dlg._show_new_content_form()
        dropped = dlg.file_drop_zone._apply_dropped_urls(
            [QUrl.fromLocalFile(first_path), QUrl.fromLocalFile(second_path)]
        )

        assert dropped is True
        assert dlg.selected_file_path == first_path
        assert dlg.file_path_input.text() == first_path

    def test_open_item_editor_keeps_text_item_in_text_form(self, dialog):
        dlg, manager = dialog
        manager.items[8] = ClipboardItem(
            id=8,
            content="普通文本内容",
            content_type="text",
            title="文本标题",
        )

        dlg.open_item_editor(8, 1)

        widgets = [
            dlg.detail_layout.itemAt(i).widget()
            for i in range(dlg.detail_layout.count())
            if dlg.detail_layout.itemAt(i).widget() is not None
        ]
        assert dlg.title_input.text() == "文本标题"
        assert dlg.content_edit.toPlainText() == "普通文本内容"
        assert any(isinstance(widget, manage_dialog_mod.QTextEdit) for widget in widgets)

    def test_save_content_updates_file_item_using_file_payload(self, dialog):
        dlg, manager = dialog
        file_group = Group(id=3, name="快速启动", icon="⚡", group_type=GroupType.FILE)
        manager.groups.append(file_group)
        original_path = os.path.normpath(r"C:\Temp\old.txt")
        updated_path = os.path.normpath(r"C:\Temp\new.txt")
        manager.items[9] = ClipboardItem(
            id=9,
            content=json.dumps({"files": [original_path]}, ensure_ascii=False),
            content_type="file",
            title="旧标题",
        )

        dlg.open_item_editor(9, file_group.id)
        dlg.file_path_input.setText(updated_path)
        dlg.selected_file_path = updated_path
        dlg.title_input.setText("新标题")

        dlg._save_content()

        assert manager.updated_items[-1] == (
            9,
            json.dumps({"files": [updated_path]}, ensure_ascii=False),
            "新标题",
        )


class TestClipboardController:
    def test_delete_group_refreshes_existing_manage_dialog(self, controller, monkeypatch):
        fake_dialog = DummyManageDialog()
        controller.current_group_id = 2
        reloads = []

        monkeypatch.setattr(clipboard_controller_mod, "get_existing_manage_dialog", lambda: fake_dialog)
        controller.reload_required.connect(lambda: reloads.append(True))

        assert controller.delete_group(2) is True
        assert controller.current_group_id is None
        assert fake_dialog.refresh_requests == [2]
        assert reloads == [True]

    def test_open_manage_dialog_for_group_uses_unique_connection(self, controller, monkeypatch):
        fake_dialog = DummyManageDialog()
        receiver = CallbackReceiver()

        monkeypatch.setattr(clipboard_controller_mod, "get_manage_dialog", lambda _manager: fake_dialog)

        controller.open_manage_dialog_for_group(7, receiver.on_group_added, receiver.on_data_changed)
        controller.open_manage_dialog_for_group(7, receiver.on_group_added, receiver.on_data_changed)
        fake_dialog.group_added.emit()
        fake_dialog.data_changed.emit()

        assert fake_dialog.group_editor_requests == [7, 7]
        assert receiver.group_added_calls == 1
        assert receiver.data_changed_calls == 1

    def test_open_manage_dialog_for_item_uses_unique_connection(self, controller, monkeypatch):
        fake_dialog = DummyManageDialog()
        receiver = CallbackReceiver()

        monkeypatch.setattr(clipboard_controller_mod, "get_manage_dialog", lambda _manager: fake_dialog)

        controller.open_manage_dialog_for_item(11, 3, receiver.on_group_added, receiver.on_data_changed)
        controller.open_manage_dialog_for_item(11, 3, receiver.on_group_added, receiver.on_data_changed)
        fake_dialog.group_added.emit()
        fake_dialog.data_changed.emit()

        assert fake_dialog.item_editor_requests == [(11, 3), (11, 3)]
        assert receiver.group_added_calls == 1
        assert receiver.data_changed_calls == 1