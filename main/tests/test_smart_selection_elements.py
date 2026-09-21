from types import SimpleNamespace

import pytest
from PySide6.QtCore import QPoint, QPointF, QRectF, Qt
from PySide6.QtGui import QImage, QWheelEvent

from canvas.scene import CanvasScene
from canvas.view import CanvasView, _rect_area
from capture.uia_element_finder import ElementIndex, UIAElementFinder, UIAElementSnapshot
from capture.window_finder import WindowFinder


def seed_chain(view, hwnd, *rects):
    """假装 hwnd 那次扫描已经回来了，按面积从细到粗给。"""
    view.element_finder._snapshots[hwnd] = ElementIndex(
        UIAElementSnapshot(rect, order) for order, rect in enumerate(rects)
    )


@pytest.fixture
def view(qapp):
    image = QImage(400, 300, QImage.Format.Format_ARGB32)
    image.fill(0xFF000000)
    scene = CanvasScene(image, QRectF(-100, -50, 400, 300))
    instance = CanvasView(scene)
    instance.smart_selection_enabled = True
    instance.smart_selection_drills_to_elements = True
    instance.window_finder = WindowFinder.__new__(WindowFinder)
    instance.window_finder.debug = False
    instance.window_finder.windows = [(100, [-80, -40, 280, 230], "top")]
    # 假装截图范围就是一块屏幕，链的最外一级由它来。
    instance.window_finder.find_monitor_rect_at_point = lambda x, y: [-100, -50, 300, 250]
    instance.element_finder = UIAElementFinder(
        instance, service=SimpleNamespace(submit=lambda request, *, urgent=False: None)
    )
    yield instance
    instance.cleanup()
    instance.deleteLater()
    scene.deleteLater()


def test_window_fallback_is_immediate_while_element_scan_is_pending(view):
    assert view._get_smart_selection_rect(QPointF(10, 10)) == QRectF(-80, -40, 360, 270)
    assert view.element_finder._inflight == {100}


def test_adjacent_controls_are_distinguished_pixel_by_pixel(view):
    seed_chain(view, 100, (10, 10, 20, 20), (20, 10, 30, 20))
    assert view._get_smart_selection_rect(QPointF(19, 12)) == QRectF(10, 10, 10, 10)
    assert view._get_smart_selection_rect(QPointF(20, 12)) == QRectF(20, 10, 10, 10)


def test_element_is_clipped_to_window_and_capture_bounds(view):
    view.window_finder.windows = [(100, [-200, -100, 280, 230], "top")]
    seed_chain(view, 100, (-300, -150, 50, 50))
    assert view._get_smart_selection_rect(QPointF(-90, -40)) == QRectF(-100, -50, 150, 100)


def test_window_only_mode_ignores_element_cache(view):
    view.smart_selection_drills_to_elements = False
    seed_chain(view, 100, (10, 10, 15, 20))
    assert view._get_smart_selection_rect(QPointF(14, 12)) == QRectF(-80, -40, 360, 270)


def test_topmost_window_prevents_hidden_window_elements_from_winning(view):
    view.window_finder.windows.append((200, [-90, -45, 290, 240], "behind"))
    seed_chain(view, 200, (10, 10, 15, 20))
    assert view._get_smart_selection_rect(QPointF(14, 12)) == QRectF(-80, -40, 360, 270)


def test_no_window_falls_back_to_the_screen_under_the_cursor(view):
    assert view._get_smart_selection_rect(QPointF(-95, -45)) == QRectF(-100, -50, 400, 300)


def test_fullscreen_window_does_not_repeat_as_a_screen_level(view):
    view.window_finder.windows = [(100, [-100, -50, 300, 250], "fullscreen")]
    seed_chain(view, 100, (10, 10, 40, 30))
    assert view._smart_selection_chain(20, 20, [-100, -50, 300, 250]) == [
        [10, 10, 40, 30], [-100, -50, 300, 250],
    ]


def test_window_wider_than_the_screen_is_the_outermost_level(view):
    view.window_finder.windows = [(100, [-100, -50, 300, 250], "across screens")]
    view.window_finder.find_monitor_rect_at_point = lambda x, y: [0, 0, 200, 200]
    assert view._smart_selection_chain(20, 20, [-100, -50, 300, 250]) == [
        [-100, -50, 300, 250],
    ]


def test_background_result_updates_stationary_cursor_preview(view, monkeypatch):
    monkeypatch.setattr("canvas.view.QCursor", SimpleNamespace(pos=lambda: QPoint(12, 12)))
    seed_chain(view, 100, (10, 10, 20, 20))
    view._on_uia_cache_updated(100)
    assert view.canvas_scene.selection_model.rect() == QRectF(10, 10, 10, 10)


@pytest.mark.parametrize("state", ["press", "drag", "confirmed", "different_window", "closed"])
def test_late_background_result_cannot_override_selection(view, monkeypatch, state):
    monkeypatch.setattr("canvas.view.QCursor", SimpleNamespace(pos=lambda: QPoint(12, 12)))
    rect = QRectF(0, 0, 100, 100)
    view.canvas_scene.selection_model.set_rect(rect)
    seed_chain(view, 100, (10, 10, 15, 20))
    if state in {"press", "drag"}:
        view.selection_drag.active = True
        view.selection_drag.dragging = state == "drag"
    elif state == "confirmed":
        view.canvas_scene.selection_model.initialize_confirmed_rect(rect)
    elif state == "closed":
        view._is_closed = True
    view._on_uia_cache_updated(200 if state == "different_window" else 100)
    assert view.canvas_scene.selection_model.rect() == rect
    view._is_closed = False


def test_element_hover_snaps_and_click_confirms_that_element(view):
    view.smart_selection_animated = True
    seed_chain(view, 100, (10, 10, 20, 20))
    view._handle_hover_preview(QPointF(12, 12))
    assert not view._smart_selection_anim.is_running
    view.selection_drag.begin(QPointF(12, 12))
    view.selection_drag.end()
    assert view.canvas_scene.selection_model.is_confirmed
    assert view.canvas_scene.selection_model.rect() == QRectF(10, 10, 10, 10)


def test_control_to_control_hover_animates_like_window_to_window(view):
    view.smart_selection_animated = True
    seed_chain(view, 100, (0, 0, 40, 40), (200, 150, 260, 210))
    view._handle_hover_preview(QPointF(10, 10))
    assert view.canvas_scene.selection_model.rect() == QRectF(0, 0, 40, 40)
    view._handle_hover_preview(QPointF(230, 180))
    assert view._smart_selection_anim.is_running
    assert view._smart_selection_anim.target == QRectF(200, 150, 60, 60)


def test_late_scan_result_animates_instead_of_jumping(view, monkeypatch):
    monkeypatch.setattr("canvas.view.QCursor", SimpleNamespace(pos=lambda: QPoint(230, 180)))
    view.smart_selection_animated = True
    view.canvas_scene.selection_model.set_rect(QRectF(-80, -40, 360, 270))
    seed_chain(view, 100, (200, 150, 260, 210))
    view._on_uia_cache_updated(100)
    assert view._smart_selection_anim.is_running
    assert view._smart_selection_anim.target == QRectF(200, 150, 60, 60)


def test_tiny_element_selects_parent_instead_of_expanding_past_its_real_bounds(view):
    seed_chain(view, 100, (10, 10, 15, 20), (0, 0, 50, 30))
    assert view._get_smart_selection_rect(QPointF(12, 12)) == QRectF(0, 0, 50, 30)


SCREEN_RECT = QRectF(-100, -50, 400, 300)
WINDOW_RECT = QRectF(-80, -40, 360, 270)
PANEL_RECT = QRectF(0, 0, 120, 90)
BUTTON_RECT = QRectF(10, 10, 30, 20)


@pytest.fixture
def nested(view):
    """按钮套在面板里，面板套在窗口里——滚轮就是沿这条链上下走。"""
    seed_chain(view, 100, (10, 10, 40, 30), (0, 0, 120, 90))
    return view


def _wheel(view, scene_pos: QPointF, steps: int = 1):
    view_pos = QPointF(view.mapFromScene(scene_pos))
    view.wheelEvent(QWheelEvent(
        view_pos, view_pos, QPoint(), QPoint(0, 120 * steps),
        Qt.MouseButton.NoButton, Qt.KeyboardModifier.NoModifier,
        Qt.ScrollPhase.ScrollUpdate, False,
    ))


def _fake_magnifier(view, zooms):
    view.magnifier_overlay = SimpleNamespace(
        cursor_scene_pos=QPointF(20, 20),
        _should_render=lambda: True,
        update_cursor=lambda scene_pos: None,
        adjust_zoom=zooms.append,
    )


def test_wheel_up_expands_through_the_chain_and_stops_at_the_screen(nested):
    nested._handle_hover_preview(QPointF(20, 20))
    assert nested.canvas_scene.selection_model.rect() == BUTTON_RECT
    for expected in (PANEL_RECT, WINDOW_RECT, SCREEN_RECT, SCREEN_RECT):
        _wheel(nested, QPointF(20, 20))
        assert nested.canvas_scene.selection_model.rect() == expected


def test_wheel_down_shrinks_back_one_level_per_notch(nested):
    nested._handle_hover_preview(QPointF(20, 20))
    for _ in range(5):
        _wheel(nested, QPointF(20, 20))
    assert nested.canvas_scene.selection_model.rect() == SCREEN_RECT
    # 滚过头的那两格不该变成回程要空滚两下
    for expected in (WINDOW_RECT, PANEL_RECT, BUTTON_RECT, BUTTON_RECT):
        _wheel(nested, QPointF(20, 20), -1)
        assert nested.canvas_scene.selection_model.rect() == expected


def test_first_notch_counts_before_any_hover_at_that_spot(nested):
    _wheel(nested, QPointF(20, 20))
    assert nested.canvas_scene.selection_model.rect() == PANEL_RECT


def test_level_survives_moving_within_the_same_control(nested):
    _wheel(nested, QPointF(20, 20))
    nested._handle_hover_preview(QPointF(30, 25))
    assert nested.canvas_scene.selection_model.rect() == PANEL_RECT


def test_level_restarts_from_the_smallest_on_another_control(nested):
    seed_chain(nested, 100, (10, 10, 40, 30), (0, 0, 120, 90),
               (200, 150, 240, 180), (150, 120, 260, 200))
    _wheel(nested, QPointF(20, 20))
    nested._handle_hover_preview(QPointF(220, 160))
    assert nested.canvas_scene.selection_model.rect() == QRectF(200, 150, 40, 30)


def test_hovering_gives_the_wheel_to_granularity_instead_of_the_magnifier(nested):
    zooms = []
    _fake_magnifier(nested, zooms)
    nested._handle_hover_preview(QPointF(20, 20))
    _wheel(nested, QPointF(20, 20))
    assert zooms == []
    assert nested.canvas_scene.selection_model.rect() == PANEL_RECT


@pytest.mark.parametrize("mode", ["off", "window"])
def test_magnifier_keeps_the_wheel_without_element_granularity(nested, mode):
    zooms = []
    _fake_magnifier(nested, zooms)
    nested.smart_selection_enabled = mode != "off"
    nested.smart_selection_drills_to_elements = False
    _wheel(nested, QPointF(20, 20))
    assert zooms == [1]


def test_magnifier_keeps_the_wheel_once_the_selection_is_confirmed(nested):
    zooms = []
    _fake_magnifier(nested, zooms)
    confirmed = QRectF(0, 0, 100, 100)
    nested.canvas_scene.selection_model.initialize_confirmed_rect(confirmed)
    _wheel(nested, QPointF(20, 20), -1)
    assert zooms == [-1]
    assert nested.canvas_scene.selection_model.rect() == confirmed


def test_wheel_stays_with_granularity_where_there_is_only_one_level(nested):
    """滚轮归谁只由检测方式的开关决定，不看鼠标底下有几级。"""
    zooms = []
    _fake_magnifier(nested, zooms)
    nested.window_finder.windows = []
    _wheel(nested, QPointF(-95, -45))
    assert zooms == []
    assert nested.canvas_scene.selection_model.rect() == SCREEN_RECT


def test_late_scan_keeps_the_level_the_user_already_scrolled_to(view, monkeypatch):
    monkeypatch.setattr("canvas.view.QCursor", SimpleNamespace(pos=lambda: QPoint(20, 20)))
    view._handle_hover_preview(QPointF(20, 20))
    _wheel(view, QPointF(20, 20))
    scrolled = view.canvas_scene.selection_model.rect()
    assert scrolled == SCREEN_RECT
    seed_chain(view, 100, (10, 10, 40, 30), (0, 0, 120, 90))
    view._on_uia_cache_updated(100)
    assert view.canvas_scene.selection_model.rect() == scrolled


def test_chain_never_steps_outwards_into_a_smaller_rect(view):
    """元素按裁剪前的面积送来，横跨边界的那个裁完可能比里面一级还小。"""
    view.window_finder.windows = [(100, [0, 0, 100, 100], "top")]
    seed_chain(view, 100, (10, 10, 50, 50), (-400, -400, 30, 30))
    chain = view._smart_selection_chain(20, 20, [-100, -50, 300, 250])
    assert [_rect_area(rect) for rect in chain] == sorted(_rect_area(r) for r in chain)
    assert chain[0] == [10, 10, 50, 50]


def test_parent_and_child_within_a_pixel_or_two_share_one_level(view):
    """border、padding 造成的一两像素之差不该各占一档——滚一格看不出区别。"""
    seed_chain(view, 100, (10, 10, 40, 30), (9, 10, 41, 31), (0, 0, 120, 90))
    chain = view._smart_selection_chain(20, 20, [-100, -50, 300, 250])
    assert chain == [[10, 10, 40, 30], [0, 0, 120, 90],
                     [-80, -40, 280, 230], [-100, -50, 300, 250]]


def _finder_with_windows(view, windows):
    view.window_finder.windows = windows
    return view.window_finder.windows_by_exposed_area()


def test_fully_covered_windows_are_never_prewarmed(view):
    """全屏窗口底下的东西，鼠标放哪都选不中，扫它们是纯浪费。"""
    assert _finder_with_windows(view, [
        (1, [0, 0, 1920, 1080], "fullscreen"),
        (2, [100, 100, 800, 600], "behind"),
        (3, [0, 0, 1920, 1080], "also behind"),
    ]) == [1]


def test_prewarm_order_follows_exposed_area_not_z_order(view):
    """Z 序靠后但露着一大块的窗口，比 Z 序靠前只露一条缝的更该预热。"""
    assert _finder_with_windows(view, [
        (1, [0, 0, 1000, 100], "thin strip on top"),
        (2, [0, 0, 1000, 1000], "mostly exposed below it"),
    ]) == [2, 1]


def test_partially_covered_window_still_counts(view):
    assert _finder_with_windows(view, [
        (1, [0, 0, 500, 500], "top"),
        (2, [400, 400, 900, 900], "overlapping"),
    ]) == [1, 2]
