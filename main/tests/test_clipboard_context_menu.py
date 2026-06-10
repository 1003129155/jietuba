# -*- coding: utf-8 -*-

from clipboard.controllers.clipboard_controller import ClipboardController
from clipboard.core import ClipboardItem, Group, GroupType


class DummyClipboardManager:
    def __init__(self, groups=None, items=None, group_items=None):
        self._groups = groups or []
        self._items = items or {}
        self._group_items = group_items or {}

    def get_groups(self):
        return list(self._groups)

    def get_item(self, item_id):
        return self._items.get(item_id)

    def get_by_group(self, group_id, offset=0, limit=1000):
        items = list(self._group_items.get(group_id, []))
        return items[offset : offset + limit]


def _make_controller(monkeypatch, manager):
    monkeypatch.setattr(ClipboardController, "_load_settings", lambda self: None)
    controller = ClipboardController(manager)
    controller.auto_paste_enabled = True
    controller.paste_with_html = True
    return controller


def _assert_separators_are_normalized(actions):
    assert actions
    assert not actions[0].is_separator
    assert not actions[-1].is_separator
    for index in range(len(actions) - 1):
        assert not (actions[index].is_separator and actions[index + 1].is_separator)


def test_build_context_menu_for_grouped_text_item_uses_rule_table(monkeypatch):
    item = ClipboardItem(id=1, content="hello", content_type="text", is_pinned=True)
    other = ClipboardItem(id=2, content="world", content_type="text")
    groups = [
        Group(id=11, name="常规", icon="A", group_type=GroupType.NORMAL),
        Group(id=12, name="文件", icon="F", group_type=GroupType.FILE),
        Group(id=13, name="隐藏", icon="H", group_type=GroupType.HIDDEN),
    ]
    manager = DummyClipboardManager(
        groups=groups,
        items={1: item, 2: other},
        group_items={100: [item, other]},
    )
    controller = _make_controller(monkeypatch, manager)
    controller.current_group_id = 100

    ctx = controller.build_context_menu_data(1)

    assert ctx is not None
    assert [action.key for action in ctx.actions if not action.is_separator] == [
        "paste",
        "toggle_pin",
        "move_group_menu",
        "edit_item",
        "move_item_up",
        "move_item_down",
        "delete_item",
    ]
    _assert_separators_are_normalized(ctx.actions)

    toggle_action = next(action for action in ctx.actions if action.key == "toggle_pin")
    assert toggle_action.label == "Unpin"

    move_up_action = next(action for action in ctx.actions if action.key == "move_item_up")
    move_down_action = next(action for action in ctx.actions if action.key == "move_item_down")
    assert move_up_action.enabled is False
    assert move_down_action.enabled is True

    move_menu = next(action for action in ctx.actions if action.key == "move_group_menu")
    assert [child.key for child in move_menu.children if not child.is_separator] == [
        "move_to_group_11",
        "remove_from_group",
    ]
    assert move_menu.children[0].translate_label is False


def test_build_context_menu_for_file_item_includes_file_groups(monkeypatch):
    item = ClipboardItem(id=7, content='{"files": ["C:/demo.txt"]}', content_type="file")
    groups = [
        Group(id=21, name="常规", icon="A", group_type=GroupType.NORMAL),
        Group(id=22, name="文件", icon="F", group_type=GroupType.FILE),
        Group(id=23, name="隐藏", icon="H", group_type=GroupType.HIDDEN),
    ]
    manager = DummyClipboardManager(groups=groups, items={7: item})
    controller = _make_controller(monkeypatch, manager)

    ctx = controller.build_context_menu_data(7)

    assert ctx is not None
    assert [action.key for action in ctx.actions if not action.is_separator] == [
        "paste",
        "toggle_pin",
        "open_file_location",
        "move_group_menu",
        "delete_item",
    ]
    _assert_separators_are_normalized(ctx.actions)

    toggle_action = next(action for action in ctx.actions if action.key == "toggle_pin")
    assert toggle_action.label == "Pin"

    move_menu = next(action for action in ctx.actions if action.key == "move_group_menu")
    assert [child.key for child in move_menu.children] == ["move_to_group_21", "move_to_group_22"]
    assert all(child.translate_label is False for child in move_menu.children)


def test_build_group_context_menu_uses_rule_table(monkeypatch):
    groups = [
        Group(id=31, name="常规", icon="A", group_type=GroupType.NORMAL),
        Group(id=32, name="工作", icon="W", group_type=GroupType.NORMAL),
    ]
    manager = DummyClipboardManager(groups=groups)
    controller = _make_controller(monkeypatch, manager)

    actions = controller.build_group_context_menu_data(31)

    assert [action.key for action in actions if not action.is_separator] == [
        "edit_group",
        "move_group_up",
        "move_group_down",
        "delete_group",
    ]
    _assert_separators_are_normalized(actions)

    move_up_action = next(action for action in actions if action.key == "move_group_up")
    move_down_action = next(action for action in actions if action.key == "move_group_down")
    assert move_up_action.enabled is False
    assert move_down_action.enabled is True