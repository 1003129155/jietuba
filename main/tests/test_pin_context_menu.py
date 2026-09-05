# -*- coding: utf-8 -*-
"""
贴图右键菜单测试（pin/pin_context_menu.py）

pin_context_menu.py 只有 102 条语句，但覆盖率仅 10.8%，而它是贴图窗口的
主要操作入口——复制、保存、翻译、旋转翻转、置顶、缩略图、关闭全在这里分发。
它最容易回归的地方是缩略图模式：那时候大半菜单项要隐藏（缩略图上没有工具栏、
没有阴影、也没法选文字），只留复制/保存/置顶/缩略图/关闭。这个 if 分支写错，
用户会在缩略图上点到一个不该出现的菜单项，然后触发一个作用在隐藏控件上的动作。

隔离方式：不构造 PinWindow，但 QAction 和 QMenu 都要求 parent 是真正的 QObject，
所以用一个真实 QWidget 当假父窗口，把被菜单连接的十几个回调作为属性挂上去。
菜单只构建不 exec()——exec() 会阻塞并真的弹窗。

两处依赖都是函数内局部 import，因此打桩目标是源模块而不是本文件命名空间：
_get_shortcut_display 里的 settings.get_tool_settings_manager，
_get_menu_style 里的 core.theme.get_theme。
"""
import pytest
from PySide6.QtCore import QPoint
from PySide6.QtWidgets import QMenu, QWidget

import pin.pin_context_menu as pin_context_menu_module
from pin.pin_context_menu import PinContextMenu, _get_shortcut_display

# 会被菜单项连接的父窗口回调
CALLBACKS = (
    "copy_to_clipboard",
    "save_image",
    "_on_translate_clicked",
    "reset_to_original_size",
    "rotate_image_cw",
    "rotate_image_ccw",
    "flip_image_horizontal",
    "flip_image_vertical",
    "reset_image_transform",
    "toggle_toolbar",
    "toggle_stay_on_top",
    "toggle_border_effect",
    "toggle_text_selection",
    "toggle_thumbnail_mode",
    "close_window",
)


class _Recorder:
    def __init__(self):
        self.calls = 0

    def __call__(self, *args):
        self.calls += 1

    @property
    def called(self):
        return self.calls > 0


@pytest.fixture(autouse=True)
def stub_theme(monkeypatch):
    """_get_menu_style 是函数内 import core.theme，桩要打在源模块上"""
    monkeypatch.setattr(
        "core.theme.get_theme",
        lambda: type("_T", (), {"theme_color_hex": "#3478f6"})())


@pytest.fixture
def stub_shortcuts(monkeypatch):
    """默认让快捷键为空，单独的用例再覆写"""
    configured = {}

    class _Cfg:
        APP_DEFAULT_SETTINGS = {}

        def get_inapp_shortcut(self, key):
            return configured.get(key, "")

    monkeypatch.setattr("settings.get_tool_settings_manager", lambda: _Cfg())
    return configured


@pytest.fixture
def parent(qapp):
    """
    真实 QWidget 作父窗口：QAction/QMenu 的第二个参数必须是 QObject。
    回调作为普通属性挂上去，逐个可观测。
    """
    widget = QWidget()
    for name in CALLBACKS:
        setattr(widget, name, _Recorder())
    return widget


def _build(parent, **state):
    menu = QMenu(parent)
    PinContextMenu(parent)._add_menu_items(menu, state)
    return menu


def _labels(menu):
    return [a.text() for a in menu.actions() if not a.isSeparator()]


def _find(menu, prefix):
    for action in menu.actions():
        if action.text().startswith(prefix):
            return action
    raise AssertionError(f"菜单里没有以 {prefix!r} 开头的项：{_labels(menu)}")


def _has(menu, prefix):
    return any(a.text().startswith(prefix) for a in menu.actions())


class TestConstruction:

    def test_only_the_parent_is_stored(self, parent):
        menu_manager = PinContextMenu(parent)
        assert menu_manager.parent is parent


class TestNormalModeStructure:

    def test_full_menu_is_offered_outside_thumbnail_mode(self, parent, stub_shortcuts):
        menu = _build(parent, thumbnail_mode=False)
        labels = _labels(menu)
        assert labels[0] == "Copy"
        assert labels[1] == "Save as"
        for prefix in ("Translate", "Reset size", "Image transform", "Toolbar",
                       "Always on top", "Shadow effect", "Text selection",
                       "Thumbnail mode", "Close"):
            assert _has(menu, prefix), prefix

    def test_close_is_the_last_entry(self, parent, stub_shortcuts):
        menu = _build(parent, thumbnail_mode=False)
        assert _labels(menu)[-1] == "Close"

    def test_the_menu_is_grouped_by_separators(self, parent, stub_shortcuts):
        menu = _build(parent, thumbnail_mode=False)
        assert sum(1 for a in menu.actions() if a.isSeparator()) == 2

    def test_image_transform_is_a_submenu(self, parent, stub_shortcuts):
        menu = _build(parent, thumbnail_mode=False)
        transform = _find(menu, "Image transform")
        assert transform.menu() is not None
        assert _labels(transform.menu()) == [
            "Rotate right", "Rotate left", "Flip horizontal",
            "Flip vertical", "Reset transform"]


class TestThumbnailModeStructure:

    def test_thumbnail_mode_keeps_only_the_universally_useful_entries(
            self, parent, stub_shortcuts):
        menu = _build(parent, thumbnail_mode=True)
        labels = _labels(menu)
        assert labels[0] == "Copy"
        assert labels[1] == "Save as"
        assert labels[-1] == "Close"
        assert _has(menu, "Always on top")
        assert _has(menu, "Thumbnail mode")

    def test_entries_that_make_no_sense_on_a_thumbnail_are_hidden(
            self, parent, stub_shortcuts):
        menu = _build(parent, thumbnail_mode=True)
        for prefix in ("Translate", "Reset size", "Image transform",
                       "Toolbar", "Shadow effect", "Text selection"):
            assert not _has(menu, prefix), prefix

    def test_thumbnail_mode_offers_exactly_five_actions(self, parent, stub_shortcuts):
        menu = _build(parent, thumbnail_mode=True)
        assert len(_labels(menu)) == 5


class TestTranslateAvailability:

    def test_translate_is_enabled_only_with_an_ocr_result(self, parent, stub_shortcuts):
        for has_result in (True, False):
            menu = _build(parent, thumbnail_mode=False, has_ocr_result=has_result)
            assert _find(menu, "Translate").isEnabled() is has_result

    def test_translate_defaults_to_disabled(self, parent, stub_shortcuts):
        """状态字典里没带这个键时保守地禁用，而不是让用户点了没反应"""
        menu = _build(parent, thumbnail_mode=False)
        assert _find(menu, "Translate").isEnabled() is False


class TestToggleMarkers:

    def test_each_toggle_shows_a_filled_or_hollow_marker(self, parent, stub_shortcuts):
        cases = {
            "Toolbar": "toolbar_visible",
            "Always on top": "stay_on_top",
            "Shadow effect": "shadow_enabled",
            "Text selection": "text_selection_enabled",
        }
        for prefix, key in cases.items():
            for enabled in (True, False):
                menu = _build(parent, thumbnail_mode=False, **{key: enabled})
                text = _find(menu, prefix).text()
                assert text.endswith("●" if enabled else "○"), (prefix, enabled)

    def test_shadow_and_text_selection_default_to_enabled(self, parent, stub_shortcuts):
        menu = _build(parent, thumbnail_mode=False)
        assert _find(menu, "Shadow effect").text().endswith("●")
        assert _find(menu, "Text selection").text().endswith("●")

    def test_toolbar_and_stay_on_top_default_to_disabled(self, parent, stub_shortcuts):
        menu = _build(parent, thumbnail_mode=False)
        assert _find(menu, "Toolbar").text().endswith("○")
        assert _find(menu, "Always on top").text().endswith("○")

    def test_thumbnail_marker_reflects_the_current_mode(self, parent, stub_shortcuts):
        for active in (True, False):
            menu = _build(parent, thumbnail_mode=active)
            assert _find(menu, "Thumbnail mode").text().endswith(
                "●" if active else "○")


class TestShortcutHints:

    def test_configured_shortcuts_are_appended_in_upper_case(self, parent, stub_shortcuts):
        stub_shortcuts["inapp_toggle_toolbar"] = "space"
        stub_shortcuts["inapp_thumbnail"] = "r"
        menu = _build(parent, thumbnail_mode=False)
        assert _find(menu, "Toolbar").text().startswith("Toolbar (SPACE)")
        assert _find(menu, "Thumbnail mode").text().startswith("Thumbnail mode (R)")

    def test_no_parenthesis_is_added_when_nothing_is_bound(self, parent, stub_shortcuts):
        menu = _build(parent, thumbnail_mode=False)
        assert "(" not in _find(menu, "Toolbar").text()
        assert "(" not in _find(menu, "Thumbnail mode").text()

    def test_display_falls_back_to_the_factory_default(self, monkeypatch):
        """用户没自定义时显示出厂默认键位，而不是空白"""
        class _Cfg:
            APP_DEFAULT_SETTINGS = {"inapp_thumbnail": "r"}

            def get_inapp_shortcut(self, key):
                return ""

        monkeypatch.setattr("settings.get_tool_settings_manager", lambda: _Cfg())
        assert _get_shortcut_display("inapp_thumbnail") == "R"

    def test_an_unknown_key_yields_an_empty_string(self, monkeypatch):
        class _Cfg:
            APP_DEFAULT_SETTINGS = {}

            def get_inapp_shortcut(self, key):
                return ""

        monkeypatch.setattr("settings.get_tool_settings_manager", lambda: _Cfg())
        assert _get_shortcut_display("inapp_nonexistent") == ""


class TestActionDispatch:

    def test_top_level_entries_reach_their_handlers(self, parent, stub_shortcuts):
        menu = _build(parent, thumbnail_mode=False, has_ocr_result=True)
        cases = {
            "Copy": "copy_to_clipboard",
            "Save as": "save_image",
            "Translate": "_on_translate_clicked",
            "Reset size": "reset_to_original_size",
            "Toolbar": "toggle_toolbar",
            "Always on top": "toggle_stay_on_top",
            "Shadow effect": "toggle_border_effect",
            "Text selection": "toggle_text_selection",
            "Thumbnail mode": "toggle_thumbnail_mode",
            "Close": "close_window",
        }
        for prefix, callback in cases.items():
            _find(menu, prefix).trigger()
            assert getattr(parent, callback).called, prefix

    def test_transform_submenu_entries_reach_their_handlers(self, parent, stub_shortcuts):
        menu = _build(parent, thumbnail_mode=False)
        submenu = _find(menu, "Image transform").menu()
        cases = {
            "Rotate right": "rotate_image_cw",
            "Rotate left": "rotate_image_ccw",
            "Flip horizontal": "flip_image_horizontal",
            "Flip vertical": "flip_image_vertical",
            "Reset transform": "reset_image_transform",
        }
        for prefix, callback in cases.items():
            _find(submenu, prefix).trigger()
            assert getattr(parent, callback).called, prefix

    def test_each_entry_triggers_only_its_own_handler(self, parent, stub_shortcuts):
        menu = _build(parent, thumbnail_mode=False)
        _find(menu, "Close").trigger()
        assert parent.close_window.calls == 1
        for name in CALLBACKS:
            if name != "close_window":
                assert getattr(parent, name).calls == 0, name

    def test_thumbnail_mode_still_dispatches_its_remaining_entries(
            self, parent, stub_shortcuts):
        menu = _build(parent, thumbnail_mode=True)
        for prefix, callback in (("Copy", "copy_to_clipboard"),
                                 ("Always on top", "toggle_stay_on_top"),
                                 ("Thumbnail mode", "toggle_thumbnail_mode"),
                                 ("Close", "close_window")):
            _find(menu, prefix).trigger()
            assert getattr(parent, callback).called, prefix


class TestMenuStyle:

    def test_the_hover_colour_follows_the_theme(self, parent):
        style = PinContextMenu(parent)._get_menu_style()
        assert "#3478f6" in style

    def test_disabled_items_get_a_muted_colour(self, parent):
        """翻译项在没有 OCR 结果时是禁用态，必须在视觉上能区分出来"""
        style = PinContextMenu(parent)._get_menu_style()
        assert "QMenu::item:disabled" in style


class TestShowSignature:

    def test_show_accepts_a_global_position_and_a_state_dict(self, parent, stub_shortcuts,
                                                             monkeypatch):
        """
        show() 末尾会 menu.exec() 打开阻塞式弹窗，测试里必须把它拦下来。

        注意不能写成 monkeypatch.setattr(QMenu, "exec", ...)：QMenu.exec 是
        Shiboken 包装的 C++ 方法，给类属性赋值不会改变调用派发，赋值本身还不
        报错——结果就是真的模态事件循环被打开，测试永远挂住。
        可行的办法是替换被测模块里的 QMenu 这个名字（普通的 Python 名字查找），
        换成一个在 Python 层覆写了 exec 的子类，菜单其余行为保持真实。
        """
        positions = []

        class _NonBlockingMenu(QMenu):
            def exec(self, pos=None):
                positions.append(pos)
                return None

        monkeypatch.setattr(pin_context_menu_module, "QMenu", _NonBlockingMenu)
        PinContextMenu(parent).show(QPoint(120, 340), {"thumbnail_mode": False})
        assert positions == [QPoint(120, 340)]
