# -*- coding: utf-8 -*-
"""
截图 / 钉图工具栏排布（顺序 + 显示方式）测试

分三层：排布归一化与持久化（纯逻辑）、工具栏按排布摆放按钮与「…」弹层、排布对话框
的编辑结果。钉图工具栏是截图工具栏的子类，却有自己的一份排布，不能跟着截图的配置变——
这条最容易被顺手破坏，单独守着。
"""
from unittest.mock import MagicMock

import pytest

from PySide6.QtCore import QEvent, QPoint, QPointF, Qt
from PySide6.QtGui import QColor, QEnterEvent, QIcon, QImage
from PySide6.QtWidgets import QApplication, QWidget

from pin.pin_toolbar import PinToolbar
from settings import get_tool_settings_manager
from ui.toolbar import Toolbar
from ui.toolbar_layout import (
    DEFAULT_ORDER, HIDE, LOCKED, MORE, PIN, SCREENSHOT, SETTING_KEYS, SHOW,
    default_layout, load_layout, normalize_layout, save_layout,
)
from ui.toolbar_layout_dialog import BUTTON_NAMES, ToolbarLayoutDialog


@pytest.fixture(autouse=True)
def _clean_layout_setting():
    """排布写在整个测试会话共享的临时配置里，每条用例前后清空，免得互相影响"""
    manager = get_tool_settings_manager()
    for key in SETTING_KEYS:
        manager.set_app_setting(key, "")
    yield
    for key in SETTING_KEYS:
        manager.set_app_setting(key, "")


def _layout_with(first=None, profile=SCREENSHOT, **modes):
    """默认顺序、全部始终显示，个别按钮换显示方式；给了 first 就把那个按钮挪到最前"""
    order = profile.order
    order = [first] + [key for key in order if key != first] if first else order
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
        assert {"confirm"} <= LOCKED
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
        assert keys[keys.index("text_recognize") + 1] == "scan_code"
        assert dict(layout)["scan_code"] == MORE


class TestPersistence:

    def test_saved_layout_round_trips(self):
        layout = _layout_with(first="pin", text=HIDE, mosaic=MORE)
        assert save_layout(layout) == layout
        assert load_layout() == layout

    def test_corrupt_config_falls_back_to_the_default_layout(self):
        get_tool_settings_manager().set_app_setting(SCREENSHOT.setting_key, "{not json")
        assert load_layout() == default_layout()


class TestScreenshotToolbar:

    def test_default_layout_folds_low_frequency_buttons_and_ends_with_more(self, qapp):
        toolbar = Toolbar()
        assert _toolbar_row(toolbar) == [
            key for key in DEFAULT_ORDER
            if key not in ("text_recognize", "scan_code", "spotlight", "watermark")
        ] + ["more"]
        assert toolbar._folded_keys == ["text_recognize", "scan_code", "spotlight", "watermark"]
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
        # 基准是同一份排布、只差 mosaic 和 text：宽度变化只来自这两个按钮
        save_layout(_layout_with(first="pin", scan_code=MORE, text_recognize=MORE))
        unfolded_width = Toolbar().width()
        save_layout(_layout_with(
            first="pin", mosaic=MORE, text=HIDE, scan_code=MORE, text_recognize=MORE))

        toolbar = Toolbar()
        row = _toolbar_row(toolbar)
        assert row[0] == "pin"
        assert row[-1] == "more"
        assert "mosaic" not in row and "text" not in row
        assert toolbar._folded_keys == ["text_recognize", "scan_code", "mosaic"]
        assert toolbar.width() < unfolded_width

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


def _pin_host():
    host = QWidget()
    host.hover_controls = MagicMock()
    return host


@pytest.fixture
def pin_window(qapp, tmp_settings, monkeypatch):
    from pin.pin_ocr_manager import PinOCRManager
    from pin.pin_window import PinWindow
    from settings.tool_settings import ToolSettingsManager

    monkeypatch.setattr(PinOCRManager, "init_now", lambda self, force=False: False)
    image = QImage(200, 150, QImage.Format.Format_ARGB32)
    image.fill(QColor(200, 200, 200))
    window = PinWindow(image, QPoint(120, 120), ToolSettingsManager(tmp_settings))
    window._show_toolbar()
    yield window
    if not window._is_closed:
        window.close_window()


class TestPinToolbar:

    def test_default_row_is_unchanged_and_new_tools_wait_in_more(self, qapp):
        toolbar = PinToolbar()
        assert _toolbar_row(toolbar) == [
            "save", "copy", "pen", "highlighter", "mosaic", "arrow", "number", "rect", "ellipse", "text",
            "eraser", "undo", "redo", "more",
        ]
        assert toolbar._folded_keys == ["spotlight", "watermark"]

    def test_pin_and_screenshot_layouts_are_separate(self, qapp):
        save_layout(_layout_with(pen=HIDE, save=MORE))
        save_layout(_layout_with(first="text", profile=PIN, mosaic=HIDE), PIN)
        row = _toolbar_row(PinToolbar())
        assert row[0] == "text" and row[-1] == "more"
        assert "mosaic" not in row
        assert {"pen", "save", "spotlight", "watermark"} <= set(row)
        assert not {"screenshot_translate", "long_screenshot", "confirm"} & set(row)

        screenshot_row = _toolbar_row(Toolbar())
        assert "pen" not in screenshot_row and "mosaic" in screenshot_row

    def test_showing_picks_up_a_layout_changed_in_another_pin(self, qapp):
        toolbar = PinToolbar()
        save_layout(_layout_with(profile=PIN, pen=HIDE), PIN)
        toolbar.show()
        assert "pen" not in _toolbar_row(toolbar)
        toolbar.hide()

    def test_hovering_the_more_popup_counts_as_hovering_the_toolbar(self, qapp):
        host = _pin_host()
        toolbar = PinToolbar(parent_pin_window=host)
        hovered = host.hover_controls.set_toolbar_hovered
        toolbar._show_more_popup()
        popup = toolbar._more_popup

        point = QPointF(1, 1)
        QApplication.sendEvent(toolbar, QEnterEvent(point, point, point))
        QApplication.sendEvent(popup, QEnterEvent(point, point, point))
        QApplication.sendEvent(toolbar, QEvent(QEvent.Type.Leave))   # 工具栏的 Leave 晚到
        assert hovered.call_args.args == (True,)

        # 点弹层里的按钮，弹层收起后不一定收到 Leave
        toolbar.spotlight_btn.click()
        assert not popup.isVisible()
        assert hovered.call_args.args == (False,)

    def test_adjusting_saves_the_pin_layout_and_keeps_the_toolbar_up(self, qapp, monkeypatch):
        host = _pin_host()
        toolbar = PinToolbar(parent_pin_window=host)
        menu_open = host.hover_controls.set_menu_open

        def accept(dialog):
            assert menu_open.call_args.args == (True,)
            assert set(dialog._rows) == set(PIN.order)
            dialog._rows["pen"].set_mode(HIDE)
            return True

        monkeypatch.setattr("ui.dialogs.exec_dialog", accept)
        toolbar._open_layout_dialog()
        assert menu_open.call_args.args == (False,)
        assert dict(load_layout(PIN))["pen"] == HIDE
        assert dict(load_layout())["pen"] == SHOW
        assert "pen" not in _toolbar_row(toolbar)

    def test_folded_spotlight_and_watermark_work_on_a_pin(self, pin_window, monkeypatch):
        from canvas.items import SpotlightItem, WatermarkItem
        from tools.watermark import WatermarkTool

        monkeypatch.setattr(WatermarkTool, "current_template", staticmethod(lambda _manager: "PIN"))
        toolbar = pin_window.toolbar
        scene = pin_window.canvas.scene

        toolbar._show_more_popup()
        toolbar.spotlight_btn.click()
        controller = scene.tool_controller
        assert controller.current_tool.id == "spotlight"
        controller.on_press(QPointF(60, 40), Qt.MouseButton.LeftButton)
        controller.on_move(QPointF(140, 110))
        controller.on_release(QPointF(140, 110))
        assert any(isinstance(item, SpotlightItem) for item in scene.items())
        image = pin_window.get_current_image()
        assert image.pixelColor(10, 10).lightness() < image.pixelColor(100, 75).lightness()

        toolbar._show_more_popup()
        toolbar.watermark_btn.click()
        item = WatermarkItem.find(scene)
        assert item is not None and item.text() == "PIN"
        assert toolbar.watermark_panel.text_field.text() == "PIN"
        scene.undo_stack.undo()
        assert WatermarkItem.find(scene) is None

    def test_closing_the_pin_deletes_the_toolbar_side_windows(self, pin_window, monkeypatch):
        """面板和「…」弹层是没有父窗口的顶层窗口，不会随工具栏一起删，关钉图时要逐个删掉"""
        import shiboken6
        from tools.watermark import WatermarkTool

        monkeypatch.setattr(WatermarkTool, "current_template", staticmethod(lambda _manager: "PIN"))
        toolbar = pin_window.toolbar
        toolbar._show_more_popup()
        toolbar.watermark_btn.click()   # 水印面板用到才建
        QApplication.processEvents()
        windows = [getattr(toolbar, attr) for attr in (*toolbar.PANEL_ATTRS, "_more_popup")]
        assert all(window is not None and window.parent() is None for window in windows)

        pin_window.close_window()
        QApplication.sendPostedEvents(None, QEvent.Type.DeferredDelete)
        assert not any(shiboken6.isValid(window) for window in windows)
        top_levels = QApplication.topLevelWidgets()
        assert not any(window is top for window in windows for top in top_levels)


class TestLayoutDialog:

    def _dialog(self, layout, profile=SCREENSHOT):
        dialog = ToolbarLayoutDialog(layout, {key: QIcon() for key in profile.order}, profile=profile)
        dialog.show()
        QApplication.processEvents()
        return dialog

    def test_every_configurable_button_has_a_row_name(self):
        assert set(SCREENSHOT.order) | set(PIN.order) <= set(BUTTON_NAMES)

    def test_pin_dialog_edits_the_pin_buttons(self, qapp):
        dialog = self._dialog(default_layout(PIN), PIN)
        assert set(dialog._rows) == set(PIN.order)
        dialog._rows["watermark"].set_mode(SHOW)
        assert dict(dialog.entries())["watermark"] == SHOW
        dialog._fill(default_layout(PIN))
        assert dialog.entries() == default_layout(PIN)
        dialog.close()

    def test_dragging_a_row_and_changing_a_mode_is_reflected_in_entries(self, qapp):
        dialog = self._dialog(default_layout())
        rows = dialog._rows

        # 把「钉图」拖到第一行上沿：其余可调整行的中线都在鼠标下方，它就该排第一
        top = rows["long_screenshot"].mapToGlobal(QPoint(0, 1)).y()
        dialog._list._drag_row(rows["pin"], top)
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

    def test_rows_fit_the_screen_and_scroll_only_when_they_dont(self, qapp):
        """锁定按钮变少后行数涨了不少：能整屏展示就不滚动，屏幕矮到放不下才滚动，但对话框不能超出屏幕"""
        dialog = self._dialog(default_layout())
        screen_height = QApplication.primaryScreen().availableGeometry().height()
        content_height = dialog._list.content_height()
        if content_height <= screen_height - 40:
            assert dialog._list.viewport().height() >= content_height - 2
            assert not dialog._list.verticalScrollBar().isVisible()
        else:
            assert dialog.height() <= screen_height
        dialog.close()

    def test_restore_defaults_resets_order_and_modes(self, qapp):
        customized = _layout_with(first="pin", text=HIDE, mosaic=MORE)
        dialog = self._dialog(customized)
        assert dialog.entries() == customized
        dialog._fill(default_layout())
        assert dialog.entries() == default_layout()
        dialog.close()
