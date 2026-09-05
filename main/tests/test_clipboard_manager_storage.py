# -*- coding: utf-8 -*-
"""
剪贴板数据层集成测试（走真实 Rust 后端）

已有的剪贴板测试要么只检查导入面（test_clipboard_api），要么明确声明
"不依赖 Rust 后端"（test_clipboard_data 只测纯数据模型属性）。
也就是说 ClipboardManager 这一层——增删改查、分组、搜索、分页、
Rust 数据结构与 Python 数据类之间的转换——从来没有被测过，342 条语句
只覆盖了 13%。

这里让管理器指向一个临时数据库，对着真实的 pyclipboard 扩展跑完整流程。
这类测试的价值在于：Rust 侧升级导致字段改名、返回结构变化时，
单元测试是发现不了的，只有真的写进去再读出来才知道。
"""
import pytest

pyclipboard = pytest.importorskip(
    "pyclipboard", reason="需要安装自制的 pyclipboard Rust 扩展包")

from clipboard.core import ClipboardItem, Group, GroupType   # noqa: E402
from clipboard.core.manager import ClipboardManager           # noqa: E402


@pytest.fixture
def manager(tmp_path, monkeypatch):
    """
    指向临时数据库的管理器。

    ClipboardManager 是单例，直接构造拿到的会是别处已经建好的那个实例，
    因此这里先清掉单例、再让它在 tmp_path 下重新开库，用完恢复现场，
    避免污染开发机上真正的剪贴板数据库。
    """
    saved_instance = ClipboardManager._instance
    ClipboardManager._instance = None

    db_path = str(tmp_path / "clipboard_test.db")
    mgr = ClipboardManager(db_path=db_path)
    if not mgr.is_available:
        pytest.skip("Rust 后端未能在临时目录打开数据库")

    yield mgr

    try:
        mgr.release_storage()
    finally:
        ClipboardManager._instance = saved_instance


# ============================================================================
# 基础存取
# ============================================================================

class TestItemRoundTrip:
    """写进去的内容要能原样读出来"""

    def test_a_new_database_starts_empty(self, manager):
        assert manager.get_history() == []
        assert manager.get_total_count() == 0

    def test_added_text_can_be_read_back(self, manager):
        item_id = manager.add_item("hello world", content_type="text")

        item = manager.get_item(item_id)
        assert item is not None
        assert item.content == "hello world"
        assert item.content_type == "text"

    def test_read_back_item_is_the_python_data_class(self, manager):
        """Rust 的结构体必须被转换成 ClipboardItem，UI 层依赖它的属性"""
        item_id = manager.add_item("x")
        assert isinstance(manager.get_item(item_id), ClipboardItem)

    def test_title_is_preserved(self, manager):
        item_id = manager.add_item("content", content_type="text", title="我的标题")
        assert manager.get_item(item_id).title == "我的标题"

    def test_unicode_content_survives_the_round_trip(self, manager):
        """中日文和 emoji 经过 Rust 层不能变形"""
        text = "中文 テスト 🎉 line\nbreak"
        item_id = manager.add_item(text)
        assert manager.get_item(item_id).content == text

    def test_count_reflects_added_items(self, manager):
        for i in range(3):
            manager.add_item(f"item {i}")
        assert manager.get_total_count() == 3

    def test_missing_item_returns_none(self, manager):
        assert manager.get_item(999999) is None


class TestUpdateAndDelete:
    def test_update_changes_the_content(self, manager):
        item_id = manager.add_item("before")

        assert manager.update_item(item_id, "after") is True
        assert manager.get_item(item_id).content == "after"

    def test_delete_removes_the_item(self, manager):
        item_id = manager.add_item("doomed")

        assert manager.delete_item(item_id) is True
        assert manager.get_item(item_id) is None
        assert manager.get_total_count() == 0

    def test_deleting_twice_does_not_blow_up(self, manager):
        item_id = manager.add_item("x")
        manager.delete_item(item_id)
        manager.delete_item(item_id)      # 不应抛异常

    def test_clear_history_empties_the_database(self, manager):
        for i in range(3):
            manager.add_item(f"item {i}")

        assert manager.clear_history() is True
        assert manager.get_total_count() == 0


class TestPinning:
    def test_toggle_pin_flips_the_flag(self, manager):
        item_id = manager.add_item("x")
        assert manager.get_item(item_id).is_pinned is False

        manager.toggle_pin(item_id)
        assert manager.get_item(item_id).is_pinned is True

        manager.toggle_pin(item_id)
        assert manager.get_item(item_id).is_pinned is False


# ============================================================================
# 历史列表
# ============================================================================

class TestHistoryListing:
    """分页与排序"""

    def test_newest_item_comes_first(self, manager):
        manager.add_item("oldest")
        manager.add_item("newest")

        history = manager.get_history()

        assert history[0].content == "newest"

    def test_limit_caps_the_number_of_rows(self, manager):
        for i in range(5):
            manager.add_item(f"item {i}")

        assert len(manager.get_history(limit=2)) == 2

    def test_offset_skips_rows(self, manager):
        for i in range(5):
            manager.add_item(f"item {i}")

        first_page = manager.get_history(offset=0, limit=2)
        second_page = manager.get_history(offset=2, limit=2)

        assert {i.id for i in first_page}.isdisjoint({i.id for i in second_page})

    def test_paging_through_everything_yields_each_row_once(self, manager):
        for i in range(5):
            manager.add_item(f"item {i}")

        seen = []
        for offset in range(0, 5, 2):
            seen.extend(i.id for i in manager.get_history(offset=offset, limit=2))

        assert len(seen) == 5
        assert len(set(seen)) == 5


class TestSearch:
    def test_search_finds_matching_content(self, manager):
        manager.add_item("alpha beta")
        manager.add_item("gamma delta")

        found = manager.search("beta")

        assert [i.content for i in found] == ["alpha beta"]

    def test_search_without_matches_returns_empty(self, manager):
        manager.add_item("alpha")
        assert manager.search("nothing-here") == []

    def test_search_matches_chinese_content(self, manager):
        manager.add_item("这是一段中文内容")
        manager.add_item("unrelated")

        found = manager.search("中文")

        assert len(found) == 1
        assert found[0].content == "这是一段中文内容"


# ============================================================================
# 分组
# ============================================================================

class TestGroups:
    def test_created_group_appears_in_the_list(self, manager):
        group_id = manager.create_group("工作")

        groups = manager.get_groups()

        assert group_id is not None
        assert "工作" in [g.name for g in groups]
        assert all(isinstance(g, Group) for g in groups)

    def test_group_can_be_renamed(self, manager):
        group_id = manager.create_group("旧名字")

        assert manager.rename_group(group_id, "新名字") is True
        assert "新名字" in [g.name for g in manager.get_groups()]

    def test_group_can_be_deleted(self, manager):
        group_id = manager.create_group("临时")

        assert manager.delete_group(group_id) is True
        assert group_id not in [g.id for g in manager.get_groups()]

    def test_item_can_be_moved_into_a_group(self, manager):
        group_id = manager.create_group("收藏")
        item_id = manager.add_item("要收藏的内容")

        assert manager.move_to_group(item_id, group_id) is True

        in_group = manager.get_by_group(group_id)
        assert [i.id for i in in_group] == [item_id]

    def test_grouping_an_item_keeps_it_in_the_history(self, manager):
        """
        条目被放进分组后仍留在历史列表里——分组是"归档/收藏"的标记，
        而不是把条目从历史中移走。clear_history(keep_grouped=True)
        正是建立在这个模型上：清空历史时把已分组的那些留下来。
        """
        group_id = manager.create_group("收藏")
        item_id = manager.add_item("要收藏的内容")
        manager.move_to_group(item_id, group_id)

        assert item_id in [i.id for i in manager.get_history()]
        assert item_id in [i.id for i in manager.get_by_group(group_id)]

    def test_item_can_be_moved_back_out_of_a_group(self, manager):
        group_id = manager.create_group("收藏")
        item_id = manager.add_item("内容")
        manager.move_to_group(item_id, group_id)

        manager.move_to_group(item_id, None)

        assert manager.get_by_group(group_id) == []

    def test_clear_history_can_keep_grouped_items(self, manager):
        """清空历史时，用户主动收藏进分组的内容不应被一起清掉"""
        group_id = manager.create_group("收藏")
        kept = manager.add_item("要保留的")
        manager.move_to_group(kept, group_id)
        manager.add_item("普通历史")

        manager.clear_history(keep_grouped=True)

        assert manager.get_item(kept) is not None
        assert [i.id for i in manager.get_by_group(group_id)] == [kept]


# ============================================================================
# 存储路径
# ============================================================================

class TestStorageLifecycle:
    def test_database_path_points_at_the_temp_file(self, manager, tmp_path):
        path = manager.get_db_path()
        assert path is not None
        assert str(tmp_path) in path

    def test_release_storage_makes_the_manager_unavailable(self, manager):
        manager.add_item("x")

        assert manager.release_storage() is True
        assert manager.is_available is False

    def test_calls_after_release_degrade_instead_of_raising(self, manager):
        """
        后端被释放（比如正在迁移数据库文件）期间，UI 仍可能调进来。
        这时应返回空值而不是抛异常把界面带崩。
        """
        manager.release_storage()

        assert manager.get_history() == []
        assert manager.get_total_count() == 0
        assert manager.get_item(1) is None
        assert manager.delete_item(1) is False

    def test_reset_storage_reopens_on_a_new_path(self, manager, tmp_path):
        manager.add_item("在旧库里")

        new_db = str(tmp_path / "another.db")
        assert manager.reset_storage(new_db) is True

        assert manager.is_available is True
        assert manager.get_total_count() == 0      # 新库是空的
        assert str(tmp_path) in manager.get_db_path()

    def test_data_written_before_a_reset_is_still_there_afterwards(self, manager, tmp_path):
        """切走再切回来，原库的数据必须还在"""
        original_db = manager.get_db_path()
        manager.add_item("持久内容")

        manager.reset_storage(str(tmp_path / "scratch.db"))
        manager.reset_storage(original_db)

        assert [i.content for i in manager.get_history()] == ["持久内容"]


class TestGroupTypeEnum:
    """
    分组类型是 IntEnum，会以整数形态存进数据库。
    这些数值一旦改动，老数据库里已有的分组类型就会被解释错，
    所以这里把取值钉住。
    """

    def test_values_are_stable(self):
        assert (GroupType.NORMAL, GroupType.FILE, GroupType.HIDDEN) == (0, 1, 2)

    def test_group_type_round_trips_through_an_int(self):
        assert GroupType(1) is GroupType.FILE
