# -*- coding: utf-8 -*-
"""剪贴板多选粘贴：选择手势、粘贴顺序、合并粘贴和批量操作。不碰真实剪贴板。"""

import threading
from datetime import datetime, timedelta
from types import SimpleNamespace
from unittest.mock import Mock

import pytest
from PySide6.QtCore import QPoint, QSettings, Qt
from PySide6.QtGui import QColor, QImage, QPainter
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication, QListWidget, QListWidgetItem, QMenu, QStyleOptionViewItem, QWidget

from clipboard.controllers.clipboard_controller import ClipboardController
from clipboard.controllers.mouse_shortcut_controller import ClipboardMouseController
from clipboard.controllers.selection_manager import SelectionManager
from clipboard.core import ClipboardItem, Group, GroupType
from clipboard.ui.panels import setting_panel
from clipboard.ui.widgets.item_delegate import ROLE_ITEM_DATA, ROLE_ITEM_ID, ClipboardItemDelegate
from clipboard.ui.windows.clipboard_window import ClipboardShortcutHandler, ClipboardWindow
from settings import get_tool_settings_manager
from settings.tool_settings import ToolSettingsManager


def _item(item_id, kind="text", content=None, pinned=False):
    return ClipboardItem(
        id=item_id,
        content=content if content is not None else f"内容 {item_id}",
        content_type=kind,
        is_pinned=pinned,
        created_at=datetime(2026, 10, 1) + timedelta(minutes=item_id),
    )


@pytest.fixture
def rows(qapp):
    widget = QListWidget()
    for item_id in (10, 11, 12, 13, 14):
        row = QListWidgetItem(str(item_id))
        row.setData(Qt.ItemDataRole.UserRole, item_id)
        widget.addItem(row)
    selection = SelectionManager(widget, lambda _item_id: None)
    changes = []
    selection.marks_changed.connect(changes.append)
    yield SimpleNamespace(widget=widget, selection=selection, changes=changes)
    selection.deleteLater()
    widget.deleteLater()


@pytest.fixture
def multi_paste_settings():
    config = get_tool_settings_manager()
    yield config
    config.set_clipboard_multi_paste_separator("\n")
    config.set_clipboard_multi_paste_order("oldest_first")
    config.set_clipboard_multi_paste_keep_merged(False)
    config.set_clipboard_move_to_top_on_paste(True)


class TestMarks:
    def test_selection_order_survives_row_changes_and_reselect(self, rows):
        selection = rows.selection
        selection.toggle_mark(12)
        selection.toggle_mark(10)
        assert selection.marked_ids(selection_order=True) == [12, 10]
        row = rows.widget.takeItem(0)
        rows.widget.addItem(row)
        assert selection.marked_ids(selection_order=True) == [12, 10]
        selection.toggle_mark(12)
        selection.toggle_mark(12)
        assert selection.marked_ids(selection_order=True) == [10, 12]

    def test_shift_range_follows_selection_direction(self, rows):
        rows.selection.toggle_mark(13)
        rows.selection.mark_range_to(10)
        assert rows.selection.marked_ids(selection_order=True) == [13, 12, 11, 10]

    def test_marks_are_listed_top_to_bottom_whatever_the_click_order(self, rows):
        for item_id in (13, 10, 12):
            rows.selection.toggle_mark(item_id)
        assert rows.selection.marked_ids() == [10, 12, 13]
        rows.selection.toggle_mark(12)
        assert rows.selection.marked_ids() == [10, 13]
        assert rows.changes == [1, 2, 3, 2]

    def test_a_range_runs_from_the_anchor_and_replaces_the_selection(self, rows):
        rows.selection.toggle_mark(11)
        rows.selection.mark_range_to(13)
        assert rows.selection.marked_ids() == [11, 12, 13]
        rows.selection.mark_range_to(10)
        assert rows.selection.marked_ids() == [10, 11]

    def test_a_range_without_an_anchor_starts_at_the_keyboard_row(self, rows):
        rows.selection.mark_range_to(11)
        assert rows.selection.marked_ids() == [11]
        rows.selection.reset()
        rows.selection.select_item_id(12)
        rows.selection.mark_range_to(14)
        assert rows.selection.marked_ids() == [12, 13, 14]

    def test_shift_arrows_extend_from_the_current_row(self, rows):
        rows.selection.select_item_id(11)
        QTest.keyClick(rows.widget, Qt.Key.Key_Down, Qt.KeyboardModifier.ShiftModifier)
        QTest.keyClick(rows.widget, Qt.Key.Key_Down, Qt.KeyboardModifier.ShiftModifier)
        assert rows.selection.marked_ids() == [11, 12, 13]
        QTest.keyClick(rows.widget, Qt.Key.Key_Up, Qt.KeyboardModifier.ShiftModifier)
        assert rows.selection.marked_ids() == [11, 12]
        # 不按 Shift 只挪当前行，已选的不变；之后再连选从新位置起
        QTest.keyClick(rows.widget, Qt.Key.Key_Down)
        assert rows.selection.marked_ids() == [11, 12]
        QTest.keyClick(rows.widget, Qt.Key.Key_Down, Qt.KeyboardModifier.ShiftModifier)
        assert rows.selection.marked_ids() == [13, 14]

    def test_shift_arrow_with_nothing_selected_marks_the_first_row(self, rows):
        QTest.keyClick(rows.widget, Qt.Key.Key_Down, Qt.KeyboardModifier.ShiftModifier)
        assert rows.selection.marked_ids() == [10]

    def test_space_toggles_the_current_row(self, rows):
        rows.selection.select_item_id(12)
        QTest.keyClick(rows.widget, Qt.Key.Key_Space)
        assert rows.selection.marked_ids() == [12]
        QTest.keyClick(rows.widget, Qt.Key.Key_Space)
        assert rows.selection.marked_ids() == []

    def test_clearing_reports_whether_anything_was_selected(self, rows):
        assert rows.selection.clear_marks() is False
        rows.selection.toggle_mark(10)
        assert rows.selection.clear_marks() is True
        assert rows.selection.marked_ids() == [] and rows.changes[-1] == 0

    def test_reopening_the_window_drops_the_selection(self, rows):
        rows.selection.toggle_mark(10)
        rows.selection.reset()
        assert rows.selection.marked_ids() == []

    def test_a_removed_row_leaves_the_selection(self, rows):
        rows.selection.toggle_mark(10)
        rows.selection.toggle_mark(12)
        window = SimpleNamespace(list_widget=rows.widget, selection_manager=rows.selection,
                                 _check_and_load_more_if_needed=lambda: None)
        ClipboardWindow._on_item_removed(window, 2)
        assert rows.selection.marked_ids() == [10]
        # 连选起点是被删的那条：起点作废，从点的这一条开始
        rows.selection.mark_range_to(13)
        assert rows.selection.marked_ids() == [13]


@pytest.fixture
def mouse_window(qtbot, tmp_path):
    window = QWidget()
    qtbot.addWidget(window)
    window.config = ToolSettingsManager(qsettings=QSettings(str(tmp_path / "mouse.ini"), QSettings.IniFormat))
    window.resize(320, 250)
    window.list_widget = QListWidget(window)
    window.list_widget.setGeometry(0, 0, 320, 250)
    window.items = {i: _item(i) for i in (1, 2, 3, 4)}
    for item_id in window.items:
        row = QListWidgetItem(str(item_id), window.list_widget)
        row.setData(Qt.UserRole, item_id)
    window._get_item_data = window.items.get
    window._on_paste_item = Mock()
    window._show_item_context_menu = Mock()
    window.selection = SelectionManager(window.list_widget, window.items.get)
    window._toggle_item_mark = window.selection.toggle_mark
    window._mark_items_to = window.selection.mark_range_to
    window.mouse = ClipboardMouseController(window)
    window.show()
    QApplication.processEvents()
    yield window
    window.mouse.cancel()
    window.selection.deleteLater()


def _click(window, row, modifiers=Qt.NoModifier, button=Qt.LeftButton):
    pos = window.list_widget.visualItemRect(window.list_widget.item(row)).center()
    QTest.mouseClick(window.list_widget.viewport(), button, modifiers, pos)


class TestMouse:
    def test_ctrl_click_selects_without_pasting(self, mouse_window):
        _click(mouse_window, 0, Qt.ControlModifier)
        _click(mouse_window, 2, Qt.ControlModifier)
        assert mouse_window.selection.marked_ids() == [1, 3]
        mouse_window._on_paste_item.assert_not_called()

    def test_shift_click_runs_from_the_row_selected_before_the_click(self, mouse_window):
        mouse_window.selection.select_item_id(2)
        _click(mouse_window, 3, Qt.ShiftModifier)
        assert mouse_window.selection.marked_ids() == [2, 3, 4]
        mouse_window._on_paste_item.assert_not_called()

    def test_a_plain_click_still_pastes(self, mouse_window):
        _click(mouse_window, 0, Qt.ControlModifier)
        _click(mouse_window, 0)
        mouse_window._on_paste_item.assert_called_once_with(1)

    def test_a_ctrl_click_bound_to_an_action_keeps_that_action(self, mouse_window):
        mouse_window.config.set_app_setting("mouse_clipboard_menu", "ctrl+left")
        _click(mouse_window, 1, Qt.ControlModifier)
        assert mouse_window._show_item_context_menu.call_args.args[0] == 2
        assert mouse_window.selection.marked_ids() == []


class _Manager:
    def __init__(self, used=None):
        self.calls = []
        self.threads = []
        self.used = used
        self.groups = []

    def paste_items(self, item_ids, **options):
        self.calls.append((list(item_ids), options))
        self.threads.append(threading.current_thread())
        return list(item_ids) if self.used is None else self.used

    def get_groups(self):
        return list(self.groups)


@pytest.fixture
def controller(monkeypatch, multi_paste_settings):
    monkeypatch.setattr(ClipboardController, "_load_settings", lambda self: None)
    ctrl = ClipboardController(_Manager())
    ctrl.current_items = [_item(5), _item(4), _item(3), _item(2)]
    ctrl.keystrokes = []
    monkeypatch.setattr(ctrl, "_send_paste_keystroke", lambda explicit: ctrl.keystrokes.append(explicit))
    return ctrl


def _ids(controller):
    return [item.id for item in controller.current_items]


class TestController:
    @pytest.mark.parametrize('group', [None, 7])
    @pytest.mark.parametrize('order, expected', [
        ('oldest_first', ['123', '456']),
        ('newest_first', ['456', '123']),
    ])
    def test_paste_follows_selection_order(self, rows, controller, multi_paste_settings, group, order, expected):
        controller.current_group_id = group
        controller.current_items = [_item(10, content='456'), _item(12, content='123')]
        multi_paste_settings.set_clipboard_multi_paste_order(order)
        rows.selection.toggle_mark(12)
        rows.selection.toggle_mark(10)
        window = _window(rows.selection)
        ClipboardWindow._paste_selected(window)
        selected = window._paste_marked.call_args.args[0]
        assert controller.paste_items(selected)
        ordered = controller.manager.calls[0][0]
        contents = {item.id: item.content for item in controller.current_items}
        assert [contents[item_id] for item_id in ordered] == expected

    @pytest.mark.parametrize("group, order, expected", [
        (None, "oldest_first", [5, 4, 3]),
        (None, "newest_first", [3, 4, 5]),
        (7, "oldest_first", [5, 4, 3]),
        (7, "newest_first", [3, 4, 5]),
    ])
    def test_paste_order(self, controller, multi_paste_settings, group, order, expected):
        """历史和分组都按传入的选择顺序排列。"""
        controller.current_group_id = group
        multi_paste_settings.set_clipboard_multi_paste_order(order)
        assert controller.multi_paste_order([5, 4, 3]) == expected

    def test_settings_reach_the_backend_and_rows_move_up_together(self, controller, multi_paste_settings):
        multi_paste_settings.set_clipboard_multi_paste_separator(", ")
        multi_paste_settings.set_clipboard_multi_paste_keep_merged(True)
        closed = []

        assert controller.paste_items([3, 2], on_close_callback=lambda: closed.append(1)) is True

        assert controller.manager.calls == [([3, 2], dict(
            with_html=True, move_to_top=True, separator=", ", plain_text=False,
            layout="vertical", keep_in_history=True,
        ))]
        assert _ids(controller) == [3, 2, 5, 4]
        assert closed == [1] and controller.keystrokes == [False]

    def test_rows_move_up_below_the_pinned_ones(self, controller):
        controller.current_items = [_item(9, pinned=True), _item(5), _item(4), _item(3)]
        controller.paste_items([5, 3])
        assert _ids(controller) == [9, 5, 3, 4]

    def test_pasting_inside_a_group_keeps_the_order(self, controller):
        controller.current_group_id = 7
        controller.paste_items([4, 2], explicit=True)
        assert controller.manager.calls[0][1]["move_to_top"] is False
        assert _ids(controller) == [5, 4, 3, 2]
        assert controller.keystrokes == [True]

    def test_nothing_written_keeps_the_window_open(self, controller):
        controller.manager.used = []
        closed = []
        assert controller.paste_items([3, 2], on_close_callback=lambda: closed.append(1)) is False
        assert closed == [] and controller.keystrokes == []

    def test_images_are_stitched_in_the_background(self, controller, qtbot):
        controller.current_items = [_item(8, "image", "[10x10]"), _item(7, "image", "[10x10]")]
        closed = []

        assert controller.paste_items([8, 7], on_close_callback=lambda: closed.append(1)) is True

        assert closed == [1]
        qtbot.waitUntil(lambda: controller.keystrokes == [False])
        assert controller.manager.threads[0] is not threading.main_thread()
        assert _ids(controller) == [8, 7]

    @pytest.mark.parametrize("group, expected", [
        (None, [3, 2, 5, 4]),
        (7, [5, 4, 3, 2]),
    ])
    def test_background_completion_only_reorders_the_history_view(
        self, controller, qtbot, monkeypatch, group, expected,
    ):
        controller.current_items = [_item(i, "image", "[10x10]") for i in (5, 4, 3, 2)]
        started = threading.Event()
        finish = threading.Event()
        paste_items = controller.manager.paste_items

        def delayed_paste(item_ids, **options):
            started.set()
            if not finish.wait(5):
                return []
            return paste_items(item_ids, **options)

        monkeypatch.setattr(controller.manager, "paste_items", delayed_paste)
        try:
            assert controller.paste_items([3, 2]) is True
            qtbot.waitUntil(started.is_set)
            controller.current_group_id = group
        finally:
            finish.set()

        qtbot.waitUntil(lambda: controller.keystrokes == [False])
        assert controller.manager.calls[0][1]["move_to_top"] is True
        assert _ids(controller) == expected

    def test_plain_text_never_stitches(self, controller):
        controller.current_items = [_item(8, "image", "[10x10]"), _item(7, "image", "[10x10]")]
        controller.paste_items([8, 7], plain_text=True)
        assert controller.manager.threads == [threading.main_thread()]

    @pytest.mark.parametrize("count, layout, fits", [
        (5, "vertical", True),     # 4000 x 15000 = 6000 万，正好在上限
        (6, "vertical", False),
        (3, "horizontal", True),
        (6, "horizontal", False),
    ])
    def test_stitched_size_limit(self, controller, count, layout, fits):
        controller.current_items = [_item(i, "image", "[4000x3000]") for i in range(count)]
        assert controller.stitched_image_fits([i for i in range(count)], layout) is fits

    def test_sizes_that_cannot_be_read_are_left_to_the_backend(self, controller):
        controller.current_items = [_item(1, "image", "[PNG 3.1 MB]"), _item(2, "image", "[90000x90000]")]
        assert controller.stitched_image_fits([1, 2]) is True

    def test_batch_delete_and_move(self, controller):
        controller.manager.delete_item = lambda item_id: item_id != 4
        controller.manager.move_to_group = lambda item_id, group_id: True
        controller.manager.get_groups = lambda: []
        assert controller.delete_items([5, 4, 3]) == 2
        assert _ids(controller) == [4, 2]
        controller.current_group_id = 1
        assert controller.move_items_to_group([4, 2], None) == 2
        assert _ids(controller) == []


class TestMarkedMenu:
    @staticmethod
    def _labels(actions):
        return [(action.label, [child.label for child in action.children]) for action in actions if not action.is_separator]

    def test_text_offers_merged_and_plain_paste(self, controller):
        controller.manager.groups = [Group(id=1, name="常用", group_type=GroupType.NORMAL)]
        menu = controller.build_marked_context_menu_data([5, 4])
        assert self._labels(menu.actions) == [
            ("Paste Selected", []),
            ("Paste Selected as Plain Text", []),
            ("Move to Group", ["常用"]),
            ("Clear Selection", []),
            ("Delete Selected", []),
        ]

    def test_images_offer_the_two_stitch_directions(self, controller):
        controller.current_items = [_item(8, "image", "[1x1]"), _item(7, "image", "[1x1]")]
        labels = [label for label, _children in self._labels(controller.build_marked_context_menu_data([8, 7]).actions)]
        assert labels[:2] == ["Stitch Vertically and Paste", "Stitch Horizontally and Paste"]

    def test_file_groups_only_take_file_selections(self, controller):
        controller.manager.groups = [
            Group(id=1, name="常用", group_type=GroupType.NORMAL),
            Group(id=2, name="文件", group_type=GroupType.FILE),
            Group(id=3, name="隐藏", group_type=GroupType.HIDDEN),
        ]
        controller.current_group_id = 1
        moves = dict(self._labels(controller.build_marked_context_menu_data([5, 4]).actions))["Move to Group"]
        assert moves == ["常用", "", "Remove from Group"]
        controller.current_items = [_item(6, "file", '{"files": ["C:\\\\a"]}'), _item(5, "file", '{"files": ["C:\\\\b"]}')]
        moves = dict(self._labels(controller.build_marked_context_menu_data([6, 5]).actions))["Move to Group"]
        assert moves[:2] == ["常用", "文件"]

    def test_rows_no_longer_loaded_give_no_menu(self, controller):
        assert controller.build_marked_context_menu_data([99]) is None


def _window(selection, **overrides):
    window = SimpleNamespace(
        _mouse_controller=None,
        selection_manager=selection,
        _paste_marked=Mock(),
        _paste_plain_text=Mock(),
        _in_file_group=lambda: False,
        _paste_close_callback=lambda: None,
        _open_file_item=Mock(),
        _get_item_data=lambda item_id: _item(item_id),
        item_pasted=Mock(),
        controller=Mock(),
    )
    for name, value in overrides.items():
        setattr(window, name, value)
    return window


class TestWindowRouting:
    def test_activating_a_selected_row_pastes_the_whole_selection(self, rows):
        rows.selection.toggle_mark(12)
        rows.selection.toggle_mark(10)
        window = _window(rows.selection)
        ClipboardWindow._on_paste_item(window, 12)
        window._paste_marked.assert_called_once_with([12, 10])

    def test_activating_another_row_pastes_just_that_row(self, rows):
        rows.selection.toggle_mark(12)
        rows.selection.toggle_mark(10)
        window = _window(rows.selection)
        ClipboardWindow._on_paste_item(window, 11)
        window._paste_marked.assert_not_called()
        window.controller.paste_item.assert_called_once()
        assert rows.selection.marked_ids() == []

    def test_enter_and_shift_enter(self, rows):
        window = _window(rows.selection)
        rows.selection.toggle_mark(11)
        rows.selection.toggle_mark(13)
        ClipboardWindow._paste_selected(window, plain_text=True)
        window._paste_marked.assert_called_once_with([11, 13], plain_text=True)

        rows.selection.clear_marks()
        rows.selection.toggle_mark(13)
        window._on_paste_item = Mock()
        ClipboardWindow._paste_selected(window)
        window._on_paste_item.assert_called_once_with(13)
        ClipboardWindow._paste_selected(window, plain_text=True)
        window._paste_plain_text.assert_called_once_with(13)

    def test_too_large_a_stitch_is_refused_before_anything_closes(self, rows, monkeypatch):
        warnings = []
        monkeypatch.setattr("clipboard.ui.windows.clipboard_window.show_warning_dialog",
                            lambda *args: warnings.append(args))
        window = _window(rows.selection, _get_item_data=lambda item_id: _item(item_id, "image", "[1x1]"),
                         tr=lambda text: text)
        window.controller.stitched_image_fits.return_value = False
        ClipboardWindow._paste_marked(window, [10, 11])
        assert len(warnings) == 1
        window.controller.paste_items.assert_not_called()

    def test_a_file_group_opens_each_selected_file(self, rows):
        rows.selection.toggle_mark(10)
        window = _window(rows.selection, _in_file_group=lambda: True)
        ClipboardWindow._paste_marked(window, [10, 11])
        assert [call.args[0] for call in window._open_file_item.call_args_list] == [10, 11]
        window.controller.paste_items.assert_not_called()
        # 右键菜单里明确点「粘贴」时照常合并粘贴
        ClipboardWindow._paste_marked(window, [10, 11], explicit=True)
        window.controller.paste_items.assert_called_once()

    def test_right_click_on_the_selection_opens_the_batch_menu(self, rows):
        rows.selection.toggle_mark(10)
        rows.selection.toggle_mark(11)
        window = _window(rows.selection, _show_marked_context_menu=Mock())
        ClipboardWindow._show_item_context_menu(window, 11, QPoint(1, 1))
        window._show_marked_context_menu.assert_called_once_with([10, 11], QPoint(1, 1))

    def test_esc_clears_the_selection_first(self, rows, monkeypatch):
        monkeypatch.setattr(QApplication, "focusWidget", staticmethod(lambda: None))
        rows.selection.toggle_mark(10)
        window = _window(rows.selection, quick_edit_popup=None, close=Mock(), search_input=object())

        class _Event:
            def key(self):
                return Qt.Key.Key_Escape

            def modifiers(self):
                return Qt.KeyboardModifier.NoModifier

        handler = ClipboardShortcutHandler(window)
        assert handler.handle_key(_Event()) is True
        assert rows.selection.marked_ids() == []
        window.close.assert_not_called()


def test_selected_rows_get_the_bar_and_hovered_ones_only_the_tint(qapp):
    from clipboard.ui.theme.themes import get_theme_manager

    theme = get_theme_manager().get_current_theme()
    widget = QListWidget()
    for item_id in (1, 2):
        row = QListWidgetItem()
        row.setData(ROLE_ITEM_ID, item_id)
        row.setData(ROLE_ITEM_DATA, _item(item_id))
        widget.addItem(row)
    delegate = ClipboardItemDelegate(parent=widget, theme=theme, show_shortcuts=False)
    bar = QColor(theme.colors.border_selected)

    def left_edge(row, marked, highlighted):
        delegate.set_marked_ids(marked)
        delegate.set_highlighted_id(highlighted)
        image = QImage(200, 40, QImage.Format.Format_ARGB32)
        image.fill(Qt.GlobalColor.white)
        painter = QPainter(image)
        option = QStyleOptionViewItem()
        option.rect = image.rect()
        delegate.paint(painter, option, widget.model().index(row, 0))
        painter.end()
        return image.pixelColor(1, 20)

    assert left_edge(0, [], 1) == bar          # 没有多选时，悬停行照旧有指示条
    assert left_edge(0, [1], 2) == bar         # 选中的行
    assert left_edge(1, [1], 2) != bar         # 有多选时，悬停的行只变底色
    widget.deleteLater()


class TestSettingsMenu:
    def test_separators_survive_the_text_box(self):
        for value in ("\n", "\n---\n", "\t|\t", "a\\nb", ""):
            assert setting_panel.unescape_separator(setting_panel.escape_separator(value)) == value
        assert setting_panel.unescape_separator(r"x\n\ty") == "x\n\ty"

    def test_menu_shows_and_saves_the_multi_paste_settings(self, qapp):
        saved = {}
        parent = QWidget()
        menu = setting_panel.show_setting_menu(
            parent, menu_style="", tr=lambda text: text,
            paste_with_html=True, auto_paste=True, close_after_paste=True, move_to_top=True,
            show_metadata=True, preserve_search=False, window_opacity=0, current_font_size=17,
            current_theme_name="light", opacity_options=[0], font_size_options=[17],
            on_toggle_paste_html=Mock(), on_toggle_auto_paste=Mock(), on_toggle_close_after_paste=Mock(),
            on_toggle_move_to_top=Mock(), on_toggle_show_metadata=Mock(), on_toggle_preserve_search=Mock(),
            on_set_opacity=Mock(), on_set_font_size=Mock(), on_set_theme=Mock(), on_add_item=Mock(),
            multi_paste_separator=" | ", multi_paste_order="newest_first", multi_paste_keep_merged=True,
            on_set_multi_paste_separator=lambda value: saved.setdefault("separator", value),
            on_set_multi_paste_order=lambda value: saved.setdefault("order", value),
            on_toggle_multi_paste_keep_merged=lambda checked: saved.setdefault("keep", checked),
            anchor_pos=QPoint(0, 0),
        )
        # 不经 QAction.menu() 取子菜单：它返回的包装随那个 QAction 包装一起失效
        submenus = {sub.title(): sub for sub in menu.findChildren(QMenu)}
        sub = {a.text(): a for a in submenus["Multi-Select Paste"].actions()}
        separators = submenus["Separator"]
        assert not any(a.isChecked() for a in separators.actions() if a.isCheckable())
        custom_edit = separators.findChild(setting_panel.LineEdit)
        assert custom_edit.text() == " | "
        order = {a.text(): a for a in submenus["Order"].actions()}
        assert order["Last Selected First"].isChecked() and not order["First Selected First"].isChecked()
        assert sub["Record Content"].isChecked()

        custom_edit.setText(r"\n--\n")
        custom_edit.returnPressed.emit()
        order["First Selected First"].trigger()
        sub["Record Content"].trigger()
        assert saved == {"separator": "\n--\n", "order": "oldest_first", "keep": False}
        menu.close()
        parent.deleteLater()


def test_multi_paste_settings_defaults_and_validation(tmp_path):
    config = ToolSettingsManager(qsettings=QSettings(str(tmp_path / "multi.ini"), QSettings.IniFormat))
    assert config.get_clipboard_multi_paste_separator() == "\n"
    assert config.get_clipboard_multi_paste_order() == "oldest_first"
    assert config.get_clipboard_multi_paste_keep_merged() is False
    for value in (", ", "", "\n\n", "\t"):
        config.set_clipboard_multi_paste_separator(value)
        assert config.get_clipboard_multi_paste_separator() == value
    config.set_clipboard_multi_paste_order("sideways")
    assert config.get_clipboard_multi_paste_order() == "oldest_first"
    config.set_clipboard_multi_paste_order("newest_first")
    config.set_clipboard_multi_paste_keep_merged(True)
    assert config.get_clipboard_multi_paste_order() == "newest_first"
    assert config.get_clipboard_multi_paste_keep_merged() is True
