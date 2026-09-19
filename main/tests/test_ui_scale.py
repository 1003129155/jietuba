# -*- coding: utf-8 -*-
"""操作界面缩放：换算规则、100% 基线、反复切换不漂移。"""
import pytest

from core.ui_scale import UIScaleManager, get_ui_scale, scaled, scaled_f


@pytest.fixture(autouse=True)
def restore_scale():
    """比例和它绑的配置都是进程级单例，用完必须还原，否则会污染后面的测试"""
    manager = get_ui_scale()
    before = manager.percent
    before_config = manager._config_manager
    yield
    manager._config_manager = None
    manager.set_percent(before)
    manager._config_manager = before_config


# ======================================================================
# 换算规则
# ======================================================================

@pytest.mark.parametrize(("given", "expected"), [
    (100, 100), (80, 80), (150, 150),
    (0, 80), (999, 150),          # 越界收到最近的档
    (97, 100), (118, 125),        # 非档位值吸附到最近的档
    (None, 100), ("abc", 100),    # 非法值回落默认
])
def test_percent_is_snapped_to_a_real_option(given, expected):
    assert UIScaleManager.normalize_percent(given) == expected


def test_zero_stays_zero_and_thin_things_never_vanish():
    """0 的间距就该是 0；1px 的分隔线缩到 0 就等于消失了"""
    get_ui_scale().set_percent(80)
    assert scaled(0) == 0
    assert scaled(1) >= 1


def test_float_helper_keeps_sub_pixel_precision():
    get_ui_scale().set_percent(150)
    assert scaled_f(2.0) == pytest.approx(3.0)
    assert scaled_f(6.0) == pytest.approx(9.0)


def test_scale_changed_only_fires_on_a_real_change():
    manager = get_ui_scale()
    manager.set_percent(100)
    seen = []
    manager.scale_changed.connect(lambda: seen.append(manager.percent))
    try:
        assert manager.set_percent(100) is False
        assert seen == []
        assert manager.set_percent(125) is True
        assert seen == [125]
    finally:
        manager.scale_changed.disconnect()


def test_sizes_are_recomputed_from_the_base_not_from_the_last_result():
    """80% 之后回到 100% 必须和一开始一模一样，不能在缩过的值上再缩"""
    manager = get_ui_scale()
    manager.set_percent(100)
    baseline = scaled(45)
    for percent in (150, 80, 125, 100):
        manager.set_percent(percent)
    assert scaled(45) == baseline


# ======================================================================
# 100% 基线 —— 吸收 0.90 之后的实际像素，和改造前一致
# ======================================================================

def test_toolbar_keeps_its_previous_size_at_100_percent(qapp):
    from ui.toolbar import Toolbar

    get_ui_scale().set_percent(100)
    toolbar = Toolbar()
    try:
        assert toolbar._btn_height == 40
        assert toolbar._button_widths["pen"] == 40
        assert toolbar._button_widths["save"] == 45
    finally:
        toolbar.deleteLater()


def test_settings_panel_keeps_its_previous_size_at_100_percent(qapp):
    from ui.shape_settings_panel import ShapeSettingsPanel

    get_ui_scale().set_percent(100)
    panel = ShapeSettingsPanel()
    try:
        assert panel.size_spin.width() == 54
        assert panel.opacity_spin.width() == 65
    finally:
        panel.deleteLater()


# ======================================================================
# 各浮层：改比例后自动跟随，来回切换不漂移
# ======================================================================

def _round_trip(widget, measure):
    manager = get_ui_scale()
    manager.set_percent(100)
    baseline = measure(widget)
    manager.set_percent(150)
    grown = measure(widget)
    manager.set_percent(80)
    shrunk = measure(widget)
    manager.set_percent(100)
    return baseline, grown, shrunk, measure(widget)


def _size_of(widget):
    return widget.width(), widget.height()


def test_toolbar_follows_the_scale_without_drifting(qapp):
    from ui.toolbar import Toolbar

    toolbar = Toolbar()
    try:
        base, grown, shrunk, back = _round_trip(toolbar, _size_of)
        assert grown[1] > base[1] > shrunk[1]
        assert back == base
    finally:
        toolbar.deleteLater()


def test_selection_info_panel_follows_the_scale_without_drifting(qapp):
    from PySide6.QtWidgets import QWidget
    from ui.selection_info import SelectionInfoPanel

    host = QWidget()
    panel = SelectionInfoPanel(host, view=None)
    try:
        base, grown, shrunk, back = _round_trip(panel, lambda w: w.height())
        assert grown > base > shrunk
        assert back == base
    finally:
        host.deleteLater()


def test_gif_toolbars_follow_the_scale_without_drifting(qapp):
    from gif.playback_toolbar import PlaybackToolbar
    from gif.record_toolbar import RecordToolbar

    record = RecordToolbar()
    playback = PlaybackToolbar()
    try:
        for toolbar in (record, playback):
            base, grown, shrunk, back = _round_trip(toolbar, lambda w: w.height())
            assert grown > base > shrunk
            assert back == base
    finally:
        record.deleteLater()
        playback.deleteLater()


def test_long_screenshot_toolbar_follows_the_scale_without_drifting(qapp):
    from stitch.scroll_toolbar import FloatingToolbar

    toolbar = FloatingToolbar()
    try:
        base, grown, shrunk, back = _round_trip(toolbar, lambda w: w.height())
        assert grown > base > shrunk
        assert back == base
    finally:
        toolbar.deleteLater()


def test_selection_info_popups_follow_the_scale_without_drifting(qapp):
    from PySide6.QtWidgets import QWidget
    from ui.selection_info import BorderShadowPopup, RoundedSliderPopup

    host = QWidget()
    popups = [BorderShadowPopup(host), RoundedSliderPopup(host)]
    try:
        for popup in popups:
            base, grown, shrunk, back = _round_trip(popup, lambda w: w.width())
            assert grown > base > shrunk
            assert back == base
    finally:
        host.deleteLater()


# ======================================================================
# 改比例只动尺寸，不动内容
# ======================================================================

def test_changing_the_scale_keeps_the_current_tool_and_panel_values(qapp):
    from ui.toolbar import Toolbar

    toolbar = Toolbar()
    try:
        toolbar.select_tool("rect")
        toolbar.shape_panel.set_size(7)
        toolbar.shape_panel.line_style = "dashed"

        get_ui_scale().set_percent(150)

        assert toolbar.current_tool == "rect"
        assert toolbar.tool_buttons["rect"].isChecked()
        assert toolbar.shape_panel.size_spin.value() == 7
        assert toolbar.shape_panel.line_style == "dashed"
    finally:
        toolbar.deleteLater()


def test_the_timeline_handle_hit_area_tracks_what_is_drawn(qapp):
    """点击判定和显示位置必须一起缩放，否则放大后点不中手柄"""
    from gif.playback_toolbar import RangeSlider

    slider = RangeSlider()
    try:
        slider.resize(300, slider.height())
        slider.set_range(100)
        get_ui_scale().set_percent(150)
        slider.apply_scale()

        handle = slider._start_handle_rect()
        assert handle.width() == scaled(RangeSlider.BASE_HANDLE_W)
        assert handle.height() == scaled(RangeSlider.BASE_HANDLE_H)
        assert slider.rect().contains(handle.center())
    finally:
        slider.deleteLater()


# ======================================================================
# 设置界面
# ======================================================================

def test_the_appearance_card_shows_the_saved_percent(qapp):
    from types import SimpleNamespace
    from PySide6.QtWidgets import QWidget
    from ui.settings_ui.components import SettingCardGroup
    from ui.settings_ui.page_appearance import _build_ui_scale_card

    get_ui_scale().set_percent(125)
    host = QWidget()
    group = SettingCardGroup("Application", host)
    dialog = SimpleNamespace(tr=lambda text: text)
    try:
        _build_ui_scale_card(dialog, group)
        assert dialog._ui_scale_combo.currentData() == 125
        assert [dialog._ui_scale_combo.itemData(i)
                for i in range(dialog._ui_scale_combo.count())] ==             list(UIScaleManager.PERCENT_OPTIONS)
    finally:
        host.deleteLater()


def test_apply_persists_the_scale_and_close_prompt_notices_it(monkeypatch, qapp, tmp_path):
    from PySide6.QtCore import QSettings
    from settings.tool_settings import ToolSettingsManager
    from ui.settings_ui.dialog import SettingsDialog

    config = ToolSettingsManager(
        qsettings=QSettings(str(tmp_path / "scale.ini"), QSettings.Format.IniFormat))
    config.set_log_dir(str(tmp_path))
    monkeypatch.setattr("ui.settings_ui.dialog.log_info", lambda *a, **k: None)
    monkeypatch.setattr(
        "core.shortcut_manager.HotkeySystem.check_hotkey_availability",
        lambda _self, _hotkey: True,
    )
    get_ui_scale().init(config)

    dialog = SettingsDialog(config)
    try:
        # 这几项会写到进程级单例或系统设置上，本用例只关心缩放
        for attr in ("log_toggle", "autostart_toggle", "language_combo",
                     "_ui_theme_combo", "_appearance_theme_color",
                     "_appearance_mask_color", "_inapp_edits"):
            if hasattr(dialog, attr):
                delattr(dialog, attr)

        dialog._settings_snapshot = dialog._snapshot_settings()
        dialog._ui_scale_combo.setCurrentIndex(
            dialog._ui_scale_combo.findData(125))
        assert dialog._has_unsaved_changes()

        dialog.accept()
        assert get_ui_scale().percent == 125
        assert config.get_app_setting("ui_scale_percent") == 125
    finally:
        dialog.deleteLater()
        qapp.processEvents()
