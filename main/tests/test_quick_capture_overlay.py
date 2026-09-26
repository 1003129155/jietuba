"""快速截图的物理像素选区和透明浮层，不读取实际桌面。"""

import ctypes
from ctypes import wintypes
from unittest.mock import Mock

import pytest
from PySide6.QtCore import QPoint, QRect, QRectF, QSettings, Qt
from PySide6.QtGui import QColor, QImage, QPainter

from canvas.items.selection_item import SelectionItem
from canvas.selection_model import SelectionModel
from core.theme import get_theme
from settings.tool_settings import ToolSettingsManager
from ui.magnifier import MagnifierOverlay
from ui.quick_capture_overlay import QuickCaptureOverlay, _disable_native_frame, selection_rect
from ui.selection_info.panel import SelectionInfoPanel
from ui.selection_overlay import SelectionOverlayWidget


def test_native_decoration_is_disabled_without_truncating_window_handle(monkeypatch):
    monkeypatch.setattr("ui.quick_capture_overlay.QGuiApplication.platformName", lambda: "windows")
    calls = []

    def set_attribute(hwnd, attribute, value, size):
        calls.append((hwnd, attribute, ctypes.cast(value, ctypes.POINTER(wintypes.DWORD)).contents.value, size))
        return -1 if attribute >= 33 else 0  # 旧版 Windows 不支持圆角和边框颜色。

    setter = Mock(side_effect=set_attribute)
    monkeypatch.setattr("ui.quick_capture_overlay.ctypes.WinDLL", Mock(return_value=Mock(DwmSetWindowAttribute=setter)))
    handle = 0x123456789ABC
    _disable_native_frame(handle)
    assert calls == [(handle, 2, 1, 4), (handle, 33, 1, 4), (handle, 34, 0xFFFFFFFE, 4)]
    assert setter.argtypes[0] == wintypes.HWND
    assert setter.restype == ctypes.c_long


def test_offscreen_platform_never_calls_dwm(monkeypatch):
    monkeypatch.setattr("ui.quick_capture_overlay.QGuiApplication.platformName", lambda: "offscreen")
    loader = Mock()
    monkeypatch.setattr("ui.quick_capture_overlay.ctypes.WinDLL", loader)
    _disable_native_frame(1)
    loader.assert_not_called()


def test_missing_dwm_is_a_safe_fallback(monkeypatch):
    monkeypatch.setattr("ui.quick_capture_overlay.QGuiApplication.platformName", lambda: "windows")
    monkeypatch.setattr("ui.quick_capture_overlay.ctypes.WinDLL", Mock(side_effect=OSError("Unavailable")))
    _disable_native_frame(1)


@pytest.mark.parametrize("start,end,expected", [
    ((20, 30), (120, 90), QRect(20, 30, 100, 60)),
    ((120, 90), (20, 30), QRect(20, 30, 100, 60)),
    ((20, 90), (120, 30), QRect(20, 30, 100, 60)),
    ((120, 30), (20, 90), QRect(20, 30, 100, 60)),
    ((20, 30), (21, 31), QRect(20, 30, 1, 1)),
    ((-100, -30), (40, 50), QRect(-100, -30, 140, 80)),
])
def test_selection_uses_exact_pixel_distances_in_each_direction(start, end, expected):
    bounds = QRect(-300, -200, 800, 600)
    assert selection_rect(QPoint(*start), QPoint(*end), bounds) == expected


@pytest.mark.parametrize("start,end", [
    ((0, 0), (0, 0)),
    ((0, 0), (0, 20)),
    ((0, 0), (20, 0)),
    ((400, 400), (500, 500)),
])
def test_empty_or_outside_selection_stays_empty(start, end):
    assert selection_rect(QPoint(*start), QPoint(*end), QRect(-100, -100, 300, 300)).isEmpty()


def test_selection_clips_against_negative_virtual_desktop_bounds():
    bounds = QRect(-200, -100, 500, 300)
    assert selection_rect(QPoint(-500, -200), QPoint(600, 400), bounds) == bounds
    assert selection_rect(QPoint(-250, -150), QPoint(20, 30), bounds) == QRect(-200, -100, 220, 130)


@pytest.fixture
def overlay(qapp, monkeypatch, tmp_path):
    monkeypatch.setattr("ui.quick_capture_overlay.set_window_exclude_from_capture", Mock(return_value=True))
    config = ToolSettingsManager(QSettings(str(tmp_path / "quick.ini"), QSettings.Format.IniFormat))
    widget = QuickCaptureOverlay(config)
    yield widget
    widget.close()
    widget.deleteLater()


def test_overlay_never_accepts_input_or_focus(overlay):
    assert overlay.parentWidget() is None
    for flag in (
        Qt.WindowType.FramelessWindowHint,
        Qt.WindowType.WindowStaysOnTopHint,
        Qt.WindowType.WindowTransparentForInput,
        Qt.WindowType.WindowDoesNotAcceptFocus,
        Qt.WindowType.NoDropShadowWindowHint,
    ):
        assert overlay.windowFlags() & flag
    for attribute in (
        Qt.WidgetAttribute.WA_TranslucentBackground,
        Qt.WidgetAttribute.WA_NoSystemBackground,
        Qt.WidgetAttribute.WA_TransparentForMouseEvents,
        Qt.WidgetAttribute.WA_ShowWithoutActivating,
    ):
        assert overlay.testAttribute(attribute)
    assert overlay.focusPolicy() == Qt.FocusPolicy.NoFocus
    assert not overlay.autoFillBackground()


def test_reuses_normal_capture_widgets_without_a_frozen_background(overlay, qapp):
    selection = QRect(70, 90, 160, 100)
    overlay.show_selection(selection.topLeft(), QPoint(230, 190), QRect(0, 0, 400, 300))
    assert isinstance(overlay.selection_item, SelectionItem)
    assert isinstance(overlay.selection_overlay, SelectionOverlayWidget)
    assert isinstance(overlay.info_panel, SelectionInfoPanel)
    assert isinstance(overlay.magnifier_overlay, MagnifierOverlay)
    assert overlay.scene.background is None
    qapp.processEvents()
    image = QImage(overlay.size(), QImage.Format.Format_ARGB32_Premultiplied)
    image.fill(Qt.GlobalColor.transparent)
    overlay.render(image)

    def pixel_at(global_x, global_y):
        return image.pixelColor(global_x - overlay.x(), global_y - overlay.y())

    # 框内每个远离边框的像素均透明，没有普通截图的底图或灰色遮罩。
    assert all(pixel_at(x, y).alpha() == 0
               for y in range(100, 180) for x in range(80, 220))
    assert pixel_at(70, 130) == get_theme().theme_color
    assert pixel_at(300, 30).alpha() == 0
    assert "70,90" in overlay.info_panel._info_label.text()
    assert "160 × 100 px" in overlay.info_panel._info_label.text()
    assert not overlay.info_panel.btn_border.isVisible()
    assert not overlay.magnifier_overlay.isVisible(), "收到真实采样前不画假色值"


@pytest.mark.parametrize("width,color", [(1, "#E91E63"), (4, "#40E0D0"), (8, "#5267DA")])
def test_frame_pixels_match_normal_capture_selection_renderer(overlay, monkeypatch, width, color):
    theme = get_theme()
    monkeypatch.setattr(theme, "_theme_color", QColor(color))
    monkeypatch.setattr(theme, "_selection_border_width", width)
    bounds = QRect(-100, -80, 420, 320)
    rect = QRectF(-40, 20, 160, 100)
    overlay.show_selection(rect.topLeft().toPoint(), rect.bottomRight().toPoint(), bounds)
    actual = QImage(bounds.size(), QImage.Format.Format_ARGB32_Premultiplied)
    actual.fill(Qt.GlobalColor.transparent)
    overlay.selection_overlay.render(actual)

    normal_model = SelectionModel()
    normal_model.activate()
    normal_model.start_dragging()
    normal_model.set_rect(rect)
    normal_item = SelectionItem(normal_model)
    expected = QImage(bounds.size(), QImage.Format.Format_ARGB32_Premultiplied)
    expected.fill(Qt.GlobalColor.transparent)
    painter = QPainter(expected)
    painter.translate(-bounds.x(), -bounds.y())
    normal_item.render(painter)
    painter.end()
    assert actual == expected


def test_info_panel_pixels_and_position_match_normal_capture(overlay):
    overlay.show_selection(QPoint(-50, 80), QPoint(160, 180), QRect(-100, -60, 500, 400))
    rect = overlay.model.rect()
    normal_panel = SelectionInfoPanel(overlay, overlay.view)
    try:
        normal_panel.set_confirmed(False)
        normal_panel.update_info_text(rect)
        normal_panel.follow_rect(rect)
        normal_panel.show()
        assert overlay.info_panel.pos() == normal_panel.pos()
        assert overlay.info_panel.grab().toImage() == normal_panel.grab().toImage()
    finally:
        normal_panel.hide()
        normal_panel.deleteLater()


def test_overlay_tracks_reverse_drag_on_negative_monitor_and_clips(overlay):
    bounds = QRect(-300, -200, 800, 600)
    overlay.show_selection(QPoint(20, 30), QPoint(-400, -300), bounds)
    assert overlay._selection_rect == QRect(-300, -200, 320, 230)
    assert bounds.contains(overlay.geometry())
    assert "320 × 230 px" in overlay.info_panel._info_label.text()
    assert "-300,-200" in overlay.info_panel._info_label.text()


def test_size_label_uses_normal_capture_edge_placement_without_enlarging_region(overlay):
    bounds = QRect(-100, -100, 300, 250)
    overlay.show_selection(QPoint(197, -100), QPoint(200, -90), bounds)
    assert bounds.contains(overlay.geometry())
    assert overlay.model.rect() == QRectF(197, -100, 3, 10)
    # 和普通截图一样，侧边放不下时放在选区内部左上角。
    assert overlay.info_panel.y() >= 0


def test_empty_selection_clears_frame_and_info_panel(overlay):
    bounds = QRect(0, 0, 400, 300)
    overlay.show_selection(QPoint(70, 90), QPoint(230, 190), bounds)
    assert overlay.isVisible()
    overlay.show_selection(QPoint(70, 90), QPoint(70, 90), bounds)
    assert overlay.model.is_empty()
    assert not overlay.info_panel.isVisible()


def test_capture_exclusion_failure_does_not_stop_overlay(overlay, monkeypatch):
    exclude = Mock(return_value=False)
    monkeypatch.setattr("ui.quick_capture_overlay.set_window_exclude_from_capture", exclude)
    bounds = QRect(0, 0, 400, 300)
    overlay.show_selection(QPoint(70, 90), QPoint(230, 190), bounds)
    assert overlay.isVisible()
    overlay.hide()
    overlay.show_selection(QPoint(70, 90), QPoint(240, 200), bounds)
    assert overlay.isVisible()
    exclude.assert_called_once_with(int(overlay.winId()), True)


def test_hide_clears_samples_and_next_session_reloads_normal_capture_options(overlay):
    bounds = QRect(0, 0, 500, 400)
    overlay.show_selection(QPoint(20, 90), QPoint(230, 190), bounds)
    sample = QImage(64, 64, QImage.Format.Format_RGB32)
    sample.fill(QColor("#EE4455"))
    overlay.set_sample_image(sample, QRect(198, 158, 64, 64))
    assert overlay.magnifier_overlay.isVisible()
    overlay.hide()
    assert not overlay._sample_ready
    assert overlay.model.is_empty()
    assert overlay.magnifier_overlay._sample_image is None
    overlay.set_sample_image(sample, QRect(198, 158, 64, 64))
    assert not overlay.magnifier_overlay.isVisible(), "结束后的迟到样本不重新显示"

    overlay.config_manager.set_app_setting("screenshot_info_hide_on_drag", True)
    overlay.config_manager.set_app_setting("magnifier_enabled", False)
    overlay.config_manager.set_app_setting("magnifier_grid", True)
    overlay.config_manager.set_app_setting("magnifier_hint", False)
    overlay.show_selection(QPoint(20, 90), QPoint(230, 190), bounds)
    overlay.set_sample_image(sample, QRect(198, 158, 64, 64))
    assert not overlay.info_panel.isVisible()
    assert not overlay.magnifier_overlay.isVisible()
    assert overlay.magnifier_overlay._show_grid
    assert not overlay.magnifier_overlay._show_hint


def test_pointer_outside_latest_sample_hides_stale_color_until_new_sample(overlay):
    bounds = QRect(0, 0, 500, 400)
    overlay.show_selection(QPoint(20, 90), QPoint(230, 190), bounds)
    sample = QImage(64, 64, QImage.Format.Format_RGB32)
    sample.fill(QColor("#EE4455"))
    overlay.set_sample_image(sample, QRect(198, 158, 64, 64))
    assert overlay.magnifier_overlay.isVisible()
    overlay.show_selection(QPoint(20, 90), QPoint(262, 190), bounds)
    assert not overlay.magnifier_overlay.isVisible(), "采样矩形右边界是排除端点，不能显示白色假像素"
    overlay.show_selection(QPoint(20, 90), QPoint(350, 290), bounds)
    assert not overlay.magnifier_overlay.isVisible()
    overlay.set_sample_image(sample, QRect(318, 258, 64, 64))
    assert overlay.magnifier_overlay.isVisible()


def test_pointer_near_patch_edge_waits_for_complete_magnifier_sample(overlay):
    bounds = QRect(-100, -100, 500, 400)
    overlay.show_selection(QPoint(-80, -80), QPoint(-20, 0), bounds)
    overlay.magnifier_overlay._zoom_factor = 2.0
    sample = QImage(96, 96, QImage.Format.Format_RGB32)
    sample.fill(QColor("#EE4455"))
    overlay.set_sample_image(sample, QRect(-68, -48, 96, 96))
    assert overlay.magnifier_overlay.isVisible()
    overlay.show_selection(QPoint(-80, -80), QPoint(24, 0), bounds)
    assert not overlay.magnifier_overlay.isVisible(), "仅中心像素在patch内不足以显示正确倍率"
    overlay.set_sample_image(sample, QRect(-24, -48, 96, 96))
    assert overlay.magnifier_overlay.isVisible()


def test_actual_desktop_edge_allows_the_same_clipped_sample_as_normal_capture(overlay):
    bounds = QRect(-100, -100, 500, 400)
    overlay.show_selection(QPoint(20, 20), QPoint(-100, -100), bounds)
    overlay.magnifier_overlay._zoom_factor = 2.0
    sample = QImage(48, 48, QImage.Format.Format_RGB32)
    sample.fill(QColor("#EE4455"))
    overlay.set_sample_image(sample, QRect(-100, -100, 48, 48))
    assert overlay.magnifier_overlay.isVisible(), "真实屏幕边缘裁切与普通截图一致"


def test_repeated_drag_frames_keep_magnifier_above_overlapping_info_panel(overlay):
    bounds = QRect(0, 0, 500, 400)
    start, end = QPoint(280, 270), QPoint(450, 390)
    overlay.show_selection(start, end, bounds)
    sample = QImage(96, 96, QImage.Format.Format_RGB32)
    sample.fill(QColor("#EE4455"))
    overlay.set_sample_image(sample, QRect(402, 342, 96, 96))
    assert overlay.info_panel.geometry().intersects(overlay.magnifier_overlay.geometry())
    before = overlay.grab().toImage()
    overlay.show_selection(start, end, bounds)
    assert overlay.grab().toImage() == before, "更新坐标不能把面板提到放大镜上方"


def test_stationary_drag_does_not_relayout_or_repaint_unchanged_overlays(overlay, monkeypatch):
    bounds = QRect(0, 0, 500, 400)
    start, end = QPoint(20, 90), QPoint(230, 190)
    overlay.show_selection(start, end, bounds)
    sample = QImage(96, 96, QImage.Format.Format_RGB32)
    sample.fill(QColor("#EE4455"))
    overlay.set_sample_image(sample, QRect(182, 142, 96, 96))
    before = overlay.grab().toImage()
    info = Mock(wraps=overlay.info_panel.update_info_text)
    geometry = Mock(wraps=overlay.setGeometry)
    cursor = Mock(wraps=overlay.magnifier_overlay.update_cursor)
    monkeypatch.setattr(overlay.info_panel, "update_info_text", info)
    monkeypatch.setattr(overlay, "setGeometry", geometry)
    monkeypatch.setattr(overlay.magnifier_overlay, "update_cursor", cursor)

    for _ in range(20):
        overlay.show_selection(start, end, bounds)
    info.assert_not_called()
    geometry.assert_not_called()
    cursor.assert_not_called()
    assert overlay.grab().toImage() == before

    # 新样本仍须更新静止光标的颜色，不能将取样刷新一起去重。
    sample.fill(QColor("#11AA77"))
    overlay.set_sample_image(sample, QRect(182, 142, 96, 96))
    cursor.assert_called_once()
    assert overlay.magnifier_overlay._sample_color(sample) == QColor("#11AA77")
    assert overlay.grab().toImage() != before


def test_drag_updates_selection_without_resetting_virtual_desktop_geometry(overlay, monkeypatch):
    bounds = QRect(-300, -200, 800, 600)
    start = QPoint(20, 30)
    overlay.show_selection(start, QPoint(-40, -50), bounds)
    geometry = Mock(wraps=overlay.setGeometry)
    child_geometry = Mock(wraps=overlay.selection_overlay.setGeometry)
    monkeypatch.setattr(overlay, "setGeometry", geometry)
    monkeypatch.setattr(overlay.selection_overlay, "setGeometry", child_geometry)
    overlay.show_selection(start, QPoint(-140, -150), bounds)
    geometry.assert_not_called()
    child_geometry.assert_not_called()
    assert overlay.model.rect() == QRectF(-140, -150, 160, 180)
    assert "160 × 180 px" in overlay.info_panel._info_label.text()
    assert "-140,-150" in overlay.info_panel._info_label.text()


def test_desktop_origin_change_repositions_unchanged_selection_and_info(overlay):
    start, end = QPoint(60, 100), QPoint(240, 200)
    overlay.show_selection(start, end, QRect(0, 0, 500, 400))
    previous_info_pos = overlay.info_panel.pos()
    bounds = QRect(-100, -60, 500, 400)
    overlay.show_selection(start, end, bounds)
    assert overlay.geometry() == bounds
    assert overlay.model.rect() == QRectF(60, 100, 180, 100)
    assert overlay.info_panel.pos() == previous_info_pos + QPoint(100, 60)

    actual = overlay.selection_overlay.grab().toImage()
    expected = QImage(bounds.size(), QImage.Format.Format_ARGB32_Premultiplied)
    expected.fill(Qt.GlobalColor.transparent)
    painter = QPainter(expected)
    painter.translate(-bounds.x(), -bounds.y())
    overlay.selection_item.render(painter)
    painter.end()
    assert actual == expected


def test_reusing_same_bounds_restores_frame_and_info_after_session_ends(overlay):
    bounds = QRect(-100, -60, 500, 400)
    start, end = QPoint(20, 90), QPoint(230, 190)
    overlay.show_selection(start, end, bounds)
    before = overlay.grab().toImage()
    overlay.hide()
    overlay.show_selection(start, end, bounds)
    assert overlay.selection_overlay.isVisible()
    assert overlay.info_panel.isVisible()
    assert overlay.model.is_dragging
    assert overlay.grab().toImage() == before


def test_commands_use_normal_magnifier_actions_and_do_not_copy_stale_color(overlay, monkeypatch):
    magnifier = overlay.magnifier_overlay
    copy = Mock()
    monkeypatch.setattr(magnifier, "copy_color_info", copy)
    overlay.handle_command("copy_color")
    copy.assert_not_called()
    overlay.show_selection(QPoint(20, 90), QPoint(230, 190), QRect(0, 0, 500, 400))
    overlay.handle_command("copy_color")
    copy.assert_not_called()
    sample = QImage(64, 64, QImage.Format.Format_RGB32)
    sample.fill(QColor("#EE4455"))
    overlay.set_sample_image(sample, QRect(198, 158, 64, 64))
    overlay.handle_command("copy_color")
    copy.assert_called_once()
    before = magnifier.get_zoom_factor()
    overlay.handle_command("zoom_in")
    assert magnifier.get_zoom_factor() == before + 0.25
    overlay.handle_command("zoom_out")
    assert magnifier.get_zoom_factor() == before
    cycle = Mock()
    monkeypatch.setattr(magnifier, "cycle_color_format", cycle)
    overlay.handle_command("cycle_color")
    cycle.assert_called_once()
