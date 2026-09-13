# -*- coding: utf-8 -*-
"""
截图工具栏排布（顺序 + 显示方式）测试

分三层：排布归一化与持久化（纯逻辑）、工具栏按排布摆放按钮与「…」弹层、排布对话框
的编辑结果。钉图工具栏是截图工具栏的子类，却不能跟着截图的排布配置变——这条最容易
被顺手破坏，单独守着。
"""
import pytest

from PySide6.QtCore import QPoint
from PySide6.QtGui import QIcon
from PySide6.QtWidgets import QApplication

from pin.pin_toolbar import PinToolbar
from settings import get_tool_settings_manager
from ui.toolbar import Toolbar
from ui.toolbar_layout import (
    DEFAULT_ORDER, HIDE, LOCKED, MORE, SETTING_KEY, SHOW,
    default_layout, load_layout, normalize_layout, save_layout,
)
from ui.toolbar_layout_dialog import ToolbarLayoutDialog


@pytest.fixture(autouse=True)
def _clean_layout_setting():
    """排布写在整个测试会话共享的临时配置里，每条用例前后清空，免得互相影响"""
    manager = get_tool_settings_manager()
    manager.set_app_setting(SETTING_KEY, "")
    yield
    manager.set_app_setting(SETTING_KEY, "")


def _layout_with(first=None, **modes):
    """默认顺序、全部始终显示，个别按钮换显示方式；给了 first 就把那个按钮挪到最前"""
    order = [first] + [key for key in DEFAULT_ORDER if key != first] if first else DEFAULT_ORDER
    return [(key, modes.get(key, SHOW)) for key in order]


def _toolbar_row(toolbar):
    """工具栏上实际摆出来的按钮，按从左到右的顺序"""
    shown = [
        (button.x(), key) for key, button in toolbar._buttons.items()
        if button.parent() is toolbar and not button.isHidden()
    ]
    return [key for _x, key in sorted(shown)]


class TestNormalizeLayout:

    @pytest.mark.parametrize("stored", [None, [], "garbage", 3, {"pen": "hide"}])
    def test_missing_or_malformed_config_is_the_default_layout(self, stored):
        assert normalize_layout(stored) == default_layout()

    def test_unknown_duplicate_and_malformed_entries_are_dropped(self):
        layout = normalize_layout([
            ("mosaic", HIDE), ("nope", SHOW), ("mosaic", SHOW), "xy", 3,
            ("save", "weird"), ("scan_code", "weird"),
        ])
        assert sorted(key for key, _mode in layout) == sorted(DEFAULT_ORDER)
        assert dict(layout)["mosaic"] == HIDE       # 重复条目以第一次为准
        assert dict(layout)["save"] == SHOW         # 不认识的显示方式按这个按钮的默认显示方式
        assert dict(layout)["scan_code"] == MORE

    def test_locked_buttons_are_always_shown(self):
        assert {"arrow", "number", "rect"} <= LOCKED
        layout = dict(normalize_layout([
            (key, HIDE if index % 2 else MORE) for index, key in enumerate(LOCKED)
        ]))
        assert all(layout[key] == SHOW for key in LOCKED)

    def test_missing_button_goes_back_after_its_default_predecessor(self):
        """升级后新增的按钮不能堆到末尾，否则会出现在「确定」右边"""
        stored = [("confirm", SHOW)] + [
            (key, SHOW) for key in DEFAULT_ORDER if key not in ("confirm", "mosaic")
        ]
        keys = [key for key, _mode in normalize_layout(stored)]
        assert keys[0] == "confirm"
        assert keys[keys.index("highlighter") + 1] == "mosaic"

    def test_button_missing_from_an_old_config_gets_its_default_mode(self):
        """升级前存下的排布里没有扫码按钮：补回时按默认收进「…」，工具栏不会突然变宽"""
        stored = [(key, SHOW) for key in DEFAULT_ORDER if key != "scan_code"]
        layout = normalize_layout(stored)
        keys = [key for key, _mode in layout]
        assert keys[keys.index("screenshot_translate") + 1] == "scan_code"
        assert dict(layout)["scan_code"] == MORE


class TestPersistence:

    def test_saved_layout_round_trips(self):
        layout = _layout_with(first="pin", text=HIDE, mosaic=MORE)
        assert save_layout(layout) == layout
        assert load_layout() == layout

    def test_corrupt_config_falls_back_to_the_default_layout(self):
        get_tool_settings_manager().set_app_setting(SETTING_KEY, "{not json")
        assert load_layout() == default_layout()


class TestScreenshotToolbar:

    def test_default_layout_folds_the_scan_button_and_ends_with_more(self, qapp):
        toolbar = Toolbar()
        assert _toolbar_row(toolbar) == [key for key in DEFAULT_ORDER if key != "scan_code"] + ["more"]
        assert toolbar._folded_keys == ["scan_code"]
        assert toolbar.copy_btn.isHidden()
        geometries = [toolbar._buttons[key].geometry() for key in _toolbar_row(toolbar)]
        for left, right in zip(geometries, geometries[1:]):
            assert left.right() < right.left()
        assert all(toolbar.rect().contains(geometry) for geometry in geometries)

    def test_more_opener_is_a_strip_as_narrow_as_the_drag_handle(self, qapp):
        """「…」和左端拖动手柄一样宽、贴着右边缘，不再占一整个按钮的宽度"""
        toolbar = Toolbar()
        assert toolbar.more_btn.width() == toolbar.drag_handle.width() < toolbar.pen_btn.width()
        assert toolbar.more_btn.geometry().right() == toolbar.rect().right()

    def test_configured_layout_reorders_folds_and_hides(self, qapp):
        default_width = Toolbar().width()
        save_layout(_layout_with(first="pin", mosaic=MORE, text=HIDE, scan_code=MORE))

        toolbar = Toolbar()
        row = _toolbar_row(toolbar)
        assert row[0] == "pin"
        assert row[-1] == "more"
        assert "mosaic" not in row and "text" not in row
        assert toolbar._folded_keys == ["scan_code", "mosaic"]
        assert toolbar.width() < default_width

    def test_hiding_a_tool_only_hides_its_button(self, qapp):
        save_layout(_layout_with(mosaic=HIDE))
        toolbar = Toolbar()
        toolbar.select_tool("mosaic")
        assert toolbar.current_tool == "mosaic"
        assert toolbar.mosaic_btn.isChecked()

    def test_more_popup_hosts_folded_buttons_that_still_work(self, qapp):
        save_layout(_layout_with(mosaic=MORE, save=MORE))
        toolbar = Toolbar()
        saved = []
        toolbar.save_clicked.connect(lambda: saved.append(True))

        toolbar._show_more_popup()
        popup = toolbar._more_popup
        assert popup.isVisible()
        assert toolbar.mosaic_btn.parent() is popup and not toolbar.mosaic_btn.isHidden()
        assert toolbar.save_btn.parent() is popup

        toolbar.save_btn.click()
        assert saved == [True]
        assert not popup.isVisible()   # 点完就收起

        toolbar._show_more_popup()
        toolbar.mosaic_btn.click()
        assert toolbar.current_tool == "mosaic"
        assert not popup.isVisible()

    def test_popup_stays_open_while_the_mouse_moves_between_more_and_popup(self, qapp):
        save_layout(_layout_with(mosaic=MORE))
        toolbar = Toolbar()

        toolbar._on_more_hover(True)      # 进入「…」
        toolbar._on_more_hover(False)     # 离开「…」，穿过缝隙
        assert toolbar._more_close_timer.isActive()
        toolbar._on_more_hover(True)      # 进入弹层
        assert not toolbar._more_close_timer.isActive()
        assert toolbar._more_popup.isVisible()

    def test_unfolded_button_moves_back_onto_the_toolbar(self, qapp):
        save_layout(_layout_with(mosaic=MORE))
        toolbar = Toolbar()
        toolbar._show_more_popup()
        toolbar._hide_more_popup()

        save_layout(_layout_with())
        toolbar.reload_layout()
        assert toolbar.mosaic_btn.parent() is toolbar
        assert _toolbar_row(toolbar) == list(DEFAULT_ORDER) + ["more"]

    def test_new_session_rereads_the_layout(self, qapp):
        toolbar = Toolbar()
        save_layout(_layout_with(mosaic=HIDE))
        toolbar.reset_session_state()
        assert "mosaic" not in _toolbar_row(toolbar)


class TestPinToolbar:

    def test_pin_toolbar_ignores_the_screenshot_layout(self, qapp):
        save_layout(_layout_with(pen=HIDE, save=MORE, screenshot_translate=HIDE))
        toolbar = PinToolbar()
        assert _toolbar_row(toolbar) == list(PinToolbar.LAYOUT)
        assert "screenshot_translate" not in _toolbar_row(toolbar)
        assert toolbar.more_btn.isHidden()


class TestLayoutDialog:

    def _dialog(self, layout):
        dialog = ToolbarLayoutDialog(layout, {key: QIcon() for key in DEFAULT_ORDER})
        dialog.show()
        QApplication.processEvents()
        return dialog

    def test_dragging_a_row_and_changing_a_mode_is_reflected_in_entries(self, qapp):
        dialog = self._dialog(default_layout())
        rows = dialog._rows

        # 把「钉图」拖到第一行上沿：其余可调整行的中线都在鼠标下方，它就该排第一
        top = rows["long_screenshot"].mapToGlobal(QPoint(0, 1)).y()
        dialog._drag_row(rows["pin"], top)
        rows["mosaic"].set_mode(MORE)

        entries = dialog.entries()
        assert [key for key, _mode in entries if key not in LOCKED][0] == "pin"
        assert dict(entries)["mosaic"] == MORE
        assert all(dict(entries)[key] == SHOW for key in LOCKED)
        assert len(entries) == len(DEFAULT_ORDER)
        dialog.close()

    def test_locked_rows_are_not_shown(self, qapp):
        dialog = self._dialog(default_layout())
        assert LOCKED.isdisjoint(dialog._rows)
        assert len(dialog._rows) == len(DEFAULT_ORDER) - len(LOCKED)
        assert dialog._rows["pin"].combo.isEnabled()
        dialog.close()

    def test_all_editable_rows_fit_without_a_scrollbar(self, qapp):
        dialog = self._dialog(default_layout())
        assert dialog._scroll.viewport().height() >= dialog._card.sizeHint().height()
        assert not dialog._scroll.verticalScrollBar().isVisible()
        dialog.close()

    def test_restore_defaults_resets_order_and_modes(self, qapp):
        customized = _layout_with(first="pin", text=HIDE, mosaic=MORE)
        dialog = self._dialog(customized)
        assert dialog.entries() == customized
        dialog._fill(default_layout())
        assert dialog.entries() == default_layout()
        dialog.close()
