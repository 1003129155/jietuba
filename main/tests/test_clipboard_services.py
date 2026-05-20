# -*- coding: utf-8 -*-

import json

from clipboard.core import Group, GroupType
from clipboard.services.file_payload_service import build_file_payload, extract_first_file_path_from_content
from clipboard.services.group_service import (
    GENERAL_GROUP_ICON,
    QUICK_LAUNCH_GROUP_ICON,
    build_delete_group_confirm_message,
    get_default_group_icon,
    get_group_display_icon,
    get_toggled_default_group_icon,
    group_name_exists,
    make_unique_group_name,
)
from clipboard.services.import_export_service import (
    collect_text_export_rows,
    import_text_rows,
    read_import_rows,
    write_csv_rows,
)
from clipboard.services.manage_dialog_service import save_file_content, save_group, save_text_content


class DummyImportExportManager:
    def __init__(self):
        self.groups = [
            Group(id=1, name="工作"),
            Group(id=2, name="学习"),
        ]
        self.items_by_group = {
            1: [
                type("Item", (), {"content_type": "text", "content": "第一条", "title": "标题A"})(),
                type("Item", (), {"content_type": "file", "content": "ignored", "title": None})(),
            ],
            2: [
                type("Item", (), {"content_type": "text", "content": "第二条", "title": None})(),
            ],
        }
        self.created_groups = []
        self.added_items = []
        self.move_requests = []
        self._next_group_id = 3
        self._next_item_id = 1

    def get_groups(self):
        return list(self.groups)

    def get_by_group(self, group_id, offset=0, limit=50):
        return self.items_by_group.get(group_id, [])[offset:offset + limit]

    def create_group(self, name):
        group_id = self._next_group_id
        self._next_group_id += 1
        group = Group(id=group_id, name=name)
        self.groups.append(group)
        self.created_groups.append(group)
        return group_id

    def add_item(self, content, content_type, title=None):
        item_id = self._next_item_id
        self._next_item_id += 1
        self.added_items.append((item_id, content, content_type, title))
        return item_id

    def move_to_group(self, item_id, group_id):
        self.move_requests.append((item_id, group_id))
        return True


class LegacyGroupManager:
    def __init__(self):
        self.create_calls = []
        self.update_calls = []

    def create_group(self, name, color=None, icon=None):
        self.create_calls.append((name, color, icon))
        return 7

    def update_group(self, group_id, name, icon=None):
        self.update_calls.append((group_id, name, icon))
        return True


class DummySaveManager:
    def __init__(self):
        self.added_items = []
        self.updated_items = []
        self.move_requests = []
        self._next_item_id = 1

    def add_item(self, content, content_type, title=None):
        item_id = self._next_item_id
        self._next_item_id += 1
        self.added_items.append((item_id, content, content_type, title))
        return item_id

    def update_item(self, item_id, content, title=None):
        self.updated_items.append((item_id, content, title))
        return True

    def move_to_group(self, item_id, group_id):
        self.move_requests.append((item_id, group_id))
        return True


def test_build_file_payload_normalizes_path():
    payload = build_file_payload(r"C:\Temp\\demo.txt")

    assert json.loads(payload) == {"files": [r"C:\Temp\demo.txt"]}


def test_extract_first_file_path_from_content_reads_json_payload():
    path = extract_first_file_path_from_content('{"files": ["C:/Temp/demo.txt"]}')

    assert path.endswith("Temp\\demo.txt")


def test_extract_first_file_path_from_content_falls_back_to_raw_path():
    path = extract_first_file_path_from_content(r"C:\Temp\legacy.txt")

    assert path == r"C:\Temp\legacy.txt"


def test_group_name_exists_respects_exclude_group_id():
    groups = [
        Group(id=1, name="工作"),
        Group(id=2, name="学习"),
    ]

    assert group_name_exists(groups, "工作") is True
    assert group_name_exists(groups, "工作", exclude_group_id=1) is False
    assert group_name_exists(groups, "不存在") is False


def test_make_unique_group_name_appends_incremental_suffix():
    used_names = {"工作", "工作 (1)", "学习"}

    assert make_unique_group_name("新建", used_names) == "新建"
    assert make_unique_group_name("工作", used_names) == "工作 (2)"


def test_get_default_group_icon_distinguishes_group_type():
    assert get_default_group_icon(False) == GENERAL_GROUP_ICON
    assert get_default_group_icon(True) == QUICK_LAUNCH_GROUP_ICON


def test_get_group_display_icon_prefers_custom_icon():
    assert get_group_display_icon("W", True) == "W"
    assert get_group_display_icon(None, True) == QUICK_LAUNCH_GROUP_ICON


def test_get_toggled_default_group_icon_only_switches_defaults():
    assert get_toggled_default_group_icon(GENERAL_GROUP_ICON, True) == QUICK_LAUNCH_GROUP_ICON
    assert get_toggled_default_group_icon(QUICK_LAUNCH_GROUP_ICON, False) == GENERAL_GROUP_ICON
    assert get_toggled_default_group_icon("🚀", True) == "🚀"


def test_build_delete_group_confirm_message_uses_translator():
    message = build_delete_group_confirm_message(lambda text: f"T:{text}")

    assert message == (
        "T:Are you sure you want to delete this group?\n"
        "T:All items in the group will also be deleted."
    )


def test_collect_text_export_rows_only_includes_text_items():
    manager = DummyImportExportManager()

    rows = collect_text_export_rows(manager, page_size=1)

    assert rows == [
        ["工作", "第一条", "标题A"],
        ["学习", "第二条", ""],
    ]


def test_write_csv_rows_and_read_import_rows_round_trip(tmp_path):
    csv_path = tmp_path / "rows.csv"
    rows = [
        ["工作", "第一条内容", "标题A"],
        ["学习", "第二条内容", ""],
    ]

    write_csv_rows(csv_path, ["Group", "Content", "Title"], rows, "utf-8-sig")

    imported_rows = read_import_rows(str(csv_path))

    assert imported_rows == [
        ("工作", "第一条内容", "标题A"),
        ("学习", "第二条内容", ""),
    ]


def test_import_text_rows_reuses_existing_group_and_creates_missing_group():
    manager = DummyImportExportManager()
    rows = [
        ("工作", "已有分组内容", "标题1"),
        ("新分组", "新分组内容", "标题2"),
        ("新分组", "第二条新分组内容", ""),
    ]

    imported_count = import_text_rows(manager, rows)

    assert imported_count == 3
    assert [group.name for group in manager.created_groups] == ["新分组"]
    assert [request[1] for request in manager.move_requests] == [3, 3, 1]


def test_save_group_supports_legacy_manager_signature():
    manager = LegacyGroupManager()

    created = save_group(manager, None, "快速启动", "⚡", 1)
    updated = save_group(manager, 3, "工作", "W", 0)

    assert created.success is True
    assert created.action == "created"
    assert manager.create_calls == [("快速启动", None, "⚡")]
    assert updated.success is True
    assert updated.action == "updated"
    assert manager.update_calls == [(3, "工作", "W")]


def test_save_text_content_creates_item_and_moves_group():
    manager = DummySaveManager()

    result = save_text_content(manager, None, 5, "正文内容", "标题")

    assert result.success is True
    assert result.action == "created"
    assert manager.added_items == [(1, "正文内容", "text", "标题")]
    assert manager.move_requests == [(1, 5)]


def test_save_file_content_updates_item_with_payload():
    manager = DummySaveManager()

    result = save_file_content(manager, 9, 5, r"C:\Temp\demo.txt", "标题")

    assert result.success is True
    assert result.action == "updated"
    assert manager.updated_items[0][0] == 9
    assert json.loads(manager.updated_items[0][1]) == {"files": [r"C:\Temp\demo.txt"]}
    assert manager.updated_items[0][2] == "标题"
