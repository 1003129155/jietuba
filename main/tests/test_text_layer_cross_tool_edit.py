from math import ceil
from pathlib import Path

import pytest
from PySide6.QtCore import QEvent, QPoint, QPointF, QRectF, Qt, QTranslator
from PySide6.QtGui import (
    QColor,
    QFont,
    QImage,
    QMouseEvent,
    QPainter,
    QPainterPath,
    QPen,
    QWheelEvent,
)
from PySide6.QtWidgets import QGraphicsScene, QStyleOptionGraphicsItem, QWidget

from canvas.scene import CanvasScene
from canvas.view import CanvasView
from canvas.items import EllipseItem, RectItem, StrokeItem, TextItem
from canvas.handle_editor import HandleType, LayerEditor
from canvas.smart_edit_controller import SmartEditController
from ui.paint_settings_panel import PaintSettingsPanel
from ui.text_settings_panel import TextSettingsPanel
from ui.toolbar import Toolbar
from ui.screenshot_window import ScreenshotWindow


def _text_item(text="note", pos=QPointF(0, 0)):
    return TextItem(text, pos, QFont("Arial", 16), QColor("black"))


def _stroke(*, highlighter=False, width=5):
    path = QPainterPath(QPointF(0, 0))
    path.lineTo(QPointF(40, 0))
    return StrokeItem(path, QPen(QColor("red"), width), is_highlighter=highlighter)


def _canvas(*, cross_tool_select=False, parent=None):
    image = QImage(100, 80, QImage.Format.Format_ARGB32_Premultiplied)
    image.fill(QColor("white"))
    scene = CanvasScene(image, QRectF(0, 0, 100, 80), enable_mosaic=True)
    view = CanvasView(scene, parent, cross_tool_select=cross_tool_select)
    return scene, view


def test_text_is_above_draw_annotations_but_below_selection_overlay(qapp):
    text = _text_item()
    assert text.zValue() == TextItem.ANNOTATION_Z_VALUE == 30
    assert text.zValue() > RectItem(QRectF(0, 0, 20, 20), QPen(QColor("red"), 4)).zValue()
    assert text.zValue() > EllipseItem(QRectF(0, 0, 20, 20), QPen(QColor("red"), 4)).zValue()
    assert text.zValue() > _stroke().zValue()
    assert text.zValue() < 101


def test_text_item_can_use_normal_annotation_layer(qapp):
    text = TextItem(
        "note",
        QPointF(0, 0),
        QFont("Arial", 16),
        QColor("black"),
        always_on_top=False,
    )
    assert text.zValue() == TextItem.NORMAL_ANNOTATION_Z_VALUE == 20


@pytest.mark.parametrize(
    ("always_on_top", "expected_top_type"),
    [(True, TextItem), (False, RectItem)],
)
def test_later_shape_respects_text_layer_setting(
    qapp,
    always_on_top,
    expected_top_type,
):
    scene = QGraphicsScene()
    text = TextItem(
        "note",
        QPointF(0, 0),
        QFont("Arial", 16),
        QColor("black"),
        always_on_top=always_on_top,
    )
    rect = RectItem(QRectF(0, 0, 80, 40), QPen(QColor("red"), 4))
    scene.addItem(text)
    scene.addItem(rect)

    drawing_items = [
        item
        for item in scene.items(QPointF(2, 2))
        if isinstance(item, (TextItem, RectItem))
    ]
    assert isinstance(drawing_items[0], expected_top_type)


@pytest.mark.parametrize(("enabled", "expected_z"), [(True, 30), (False, 20)])
def test_text_tool_applies_always_on_top_setting(
    qapp,
    monkeypatch,
    enabled,
    expected_z,
):
    class Manager:
        @staticmethod
        def get_setting(_tool_id, _key, default=None):
            return default

        @staticmethod
        def get_text_always_on_top_enabled():
            return enabled

    scene, view = _canvas()
    try:
        monkeypatch.setattr("settings.get_tool_settings_manager", lambda: Manager())
        text_tool = scene.tool_controller.get_tool("text")
        text_tool.on_press(
            QPointF(10, 10),
            Qt.MouseButton.LeftButton,
            scene.tool_controller.ctx,
        )
        items = [item for item in scene.items() if isinstance(item, TextItem)]
        assert len(items) == 1
        assert items[0].zValue() == expected_z
    finally:
        view.close()
        scene.deleteLater()


def _scale_handle(editor):
    return next(h for h in editor.handles if h.handle_type == HandleType.TEXT_SCALE)


def test_text_item_hugs_its_content_so_short_text_leaves_no_spare_background(qapp):
    """短内容不能撑出固定宽度，否则开背景色时会拖出一大片多余区域。"""
    short = _text_item("a")
    longer = _text_item("这是一段明显更长的文字")

    assert short.textWidth() == -1, "不应设置换行宽度"
    assert short.sceneBoundingRect().width() < longer.sceneBoundingRect().width()
    # 高度只有一行，宽度贴着内容走
    assert short.sceneBoundingRect().width() < 60


def test_selected_text_exposes_rotate_delete_and_scale_corner_handles(qapp):
    item = _text_item("12323")
    editor = LayerEditor()
    editor.start_edit(item)

    rect = item.sceneBoundingRect()
    by_type = {h.handle_type: h for h in editor.handles}
    assert set(by_type) == {
        HandleType.ROTATE,
        HandleType.ITEM_DELETE,
        HandleType.TEXT_SCALE,
    }
    # 左上旋转、右上删除、右下缩放；左下角不放东西
    assert by_type[HandleType.ROTATE].position == rect.topLeft()
    assert by_type[HandleType.ITEM_DELETE].position == rect.topRight()
    assert by_type[HandleType.TEXT_SCALE].position == rect.bottomRight()


def test_scale_handle_grows_and_shrinks_the_font_in_both_directions(qapp):
    """原先的右边缘宽度手柄在贴到选区边界后往外拖是死的，缩放手柄两个方向都得活。"""
    item = _text_item("12323")
    editor = LayerEditor()
    editor.start_edit(item)
    base_size = item.font_point_size()

    handle = _scale_handle(editor)
    start = QPointF(handle.position)
    editor.start_drag(handle, start)
    editor.drag_to(QPointF(start.x() + 40, start.y() + 20))
    editor.end_drag()
    grown = item.font_point_size()
    assert grown > base_size

    handle = _scale_handle(editor)
    start = QPointF(handle.position)
    editor.start_drag(handle, start)
    editor.drag_to(QPointF(start.x() - 30, start.y() - 15))
    editor.end_drag()
    assert item.font_point_size() < grown


def test_scale_handle_clamps_to_the_minimum_font_size(qapp):
    item = _text_item("12323")
    editor = LayerEditor()
    editor.start_edit(item)

    handle = _scale_handle(editor)
    start = QPointF(handle.position)
    editor.start_drag(handle, start)
    editor.drag_to(QPointF(start.x() - 5000, start.y() - 5000))
    editor.end_drag()

    assert item.font_point_size() == pytest.approx(TextItem.MIN_POINT_SIZE)


def test_edit_snapshot_carries_font_size_instead_of_text_width(qapp):
    item = _text_item("12323")
    editor = LayerEditor()
    editor.start_edit(item)

    state = editor.capture_state(item)
    assert state["font_point_size"] == pytest.approx(item.font_point_size())
    # 换行宽度这套状态已经不存在了，不该再出现在快照里
    assert "text_width" not in state
    assert "manual_text_width" not in state


def test_handles_follow_the_box_while_the_text_is_being_typed(qapp):
    """打字会把框撑大，控制点必须跟着走。

    控制点原本只在 start_edit() 和拖拽结束后生成，所以边打字边看会发现它们
    悬在旧位置上不动，旧坐标还在画面上留下残影。
    """
    item = _text_item("1")
    editor = LayerEditor()
    editor.start_edit(item)

    before = {h.handle_type: QPointF(h.position) for h in editor.handles}
    narrow_right = item.sceneBoundingRect().right()

    item.setPlainText("121212121212")
    wide_right = item.sceneBoundingRect().right()
    assert wide_right > narrow_right, "内容变长后框应该变宽"

    editor.refresh_handles()
    after = {h.handle_type: QPointF(h.position) for h in editor.handles}

    rect = item.sceneBoundingRect()
    assert after[HandleType.ITEM_DELETE] == rect.topRight()
    assert after[HandleType.TEXT_SCALE] == rect.bottomRight()
    assert after[HandleType.ROTATE] == rect.topLeft()
    # 右侧两个控制点确实移动过，不是恰好相等
    assert after[HandleType.ITEM_DELETE] != before[HandleType.ITEM_DELETE]
    assert after[HandleType.TEXT_SCALE] != before[HandleType.TEXT_SCALE]


def test_render_refreshes_stale_handles_without_an_explicit_call(qapp):
    """场景前景层直接调 render()，它必须自己保证控制点是最新的。"""
    item = _text_item("1")
    scene = QGraphicsScene()
    scene.addItem(item)
    editor = LayerEditor()
    editor.start_edit(item)

    item.setPlainText("121212121212")
    stale = QPointF(editor.handles[0].position)

    image = QImage(400, 200, QImage.Format.Format_ARGB32_Premultiplied)
    image.fill(QColor("white"))
    painter = QPainter(image)
    try:
        editor.render(painter)
    finally:
        painter.end()

    rect = item.sceneBoundingRect()
    positions = {h.handle_type: QPointF(h.position) for h in editor.handles}
    assert positions[HandleType.TEXT_SCALE] == rect.bottomRight()
    assert positions[HandleType.ITEM_DELETE] == rect.topRight()
    del stale


def test_font_size_change_also_moves_the_handles(qapp):
    """改字号同样在拖拽之外改变尺寸，控制点一样要跟上。"""
    item = _text_item("12323")
    editor = LayerEditor()
    editor.start_edit(item)
    before = QPointF(
        next(h for h in editor.handles if h.handle_type == HandleType.TEXT_SCALE).position
    )

    item.set_font_point_size(item.font_point_size() * 2)
    editor.refresh_handles()

    after = next(
        h for h in editor.handles if h.handle_type == HandleType.TEXT_SCALE
    ).position
    assert after == item.sceneBoundingRect().bottomRight()
    assert after.x() > before.x() and after.y() > before.y()


def test_delete_handle_is_recognised_for_text_as_well_as_numbers(qapp):
    item = _text_item("12323")
    editor = LayerEditor()
    editor.start_edit(item)

    by_type = {h.handle_type: h for h in editor.handles}
    assert editor.is_delete_handle(by_type[HandleType.ITEM_DELETE])
    assert not editor.is_delete_handle(by_type[HandleType.TEXT_SCALE])
    assert not editor.is_delete_handle(by_type[HandleType.ROTATE])
    # 旧调用名保留
    assert editor.is_number_delete_handle(by_type[HandleType.ITEM_DELETE])


def test_active_text_edit_starts_scale_handle_drag_on_first_press(qapp):
    """文本光标已激活时，第一次按在手柄上就要开始拖，不能被文本编辑吃掉。"""
    image = QImage(400, 200, QImage.Format.Format_ARGB32_Premultiplied)
    image.fill(QColor("white"))
    scene = CanvasScene(image, QRectF(0, 0, 400, 200), enable_mosaic=True)
    view = CanvasView(scene)
    try:
        scene.selection_model.initialize_confirmed_rect(QRectF(0, 0, 400, 200))
        item = TextItem("edit me", QPointF(10, 10), QFont("Arial", 16), QColor("black"))
        scene.addItem(item)
        view._enter_text_edit_mode(item)

        editor = view.smart_edit_controller.layer_editor
        handle = _scale_handle(editor)
        view_pos = QPointF(view.mapFromScene(handle.position))
        initial_size = item.font_point_size()

        view.mousePressEvent(QMouseEvent(
            QEvent.Type.MouseButtonPress,
            view_pos,
            view.mapToGlobal(view_pos.toPoint()),
            Qt.MouseButton.LeftButton,
            Qt.MouseButton.LeftButton,
            Qt.KeyboardModifier.NoModifier,
        ))

        assert editor.dragging_handle is handle
        assert not view._text_drag_active

        drag_scene_pos = handle.position + QPointF(30, 15)
        drag_view_pos = QPointF(view.mapFromScene(drag_scene_pos))
        view.mouseMoveEvent(QMouseEvent(
            QEvent.Type.MouseMove,
            drag_view_pos,
            view.mapToGlobal(drag_view_pos.toPoint()),
            Qt.MouseButton.NoButton,
            Qt.MouseButton.LeftButton,
            Qt.KeyboardModifier.NoModifier,
        ))
        view.mouseReleaseEvent(QMouseEvent(
            QEvent.Type.MouseButtonRelease,
            drag_view_pos,
            view.mapToGlobal(drag_view_pos.toPoint()),
            Qt.MouseButton.LeftButton,
            Qt.MouseButton.NoButton,
            Qt.KeyboardModifier.NoModifier,
        ))

        assert item.font_point_size() > initial_size
        assert scene.undo_stack.canUndo()
        scene.undo_stack.undo()
        assert item.font_point_size() == pytest.approx(initial_size)
    finally:
        view.close()
        scene.deleteLater()


def test_text_background_uses_padding_and_rounded_corners(qapp):
    item = _text_item("A")
    item.setTextInteractionFlags(Qt.TextInteractionFlag.NoTextInteraction)
    item.set_background(True, QColor(255, 255, 0, 255), 255)

    rect = item.boundingRect()
    image = QImage(ceil(rect.width()), ceil(rect.height()), QImage.Format.Format_ARGB32_Premultiplied)
    image.fill(Qt.GlobalColor.transparent)
    painter = QPainter(image)
    painter.translate(-rect.topLeft())
    item.paint(painter, QStyleOptionGraphicsItem(), None)
    painter.end()

    corner = image.pixelColor(0, 0)
    padded_edge = image.pixelColor(1, image.height() // 2)
    assert corner.alpha() == 0
    assert padded_edge.red() > 240 and padded_edge.green() > 240
    assert padded_edge.blue() < 20


def test_background_toggle_previews_selected_color(qapp):
    panel = TextSettingsPanel()
    try:
        panel.background_color = QColor("#123456")
        panel.background_opacity = 210
        panel.background_btn.setChecked(True)
        panel._update_background_btn_style()
        style = panel.background_btn.styleSheet().lower()
        assert "18, 52, 86" in style or "#123456" in style
    finally:
        panel.deleteLater()


@pytest.mark.parametrize("current_tool", ["text", "cursor", "eraser", "mosaic"])
def test_ctrl_selects_any_editable_item_when_host_capability_is_enabled(qapp, current_tool):
    scene = QGraphicsScene()
    controller = SmartEditController(scene)
    controller.cross_tool_select_enabled = True
    controller.current_tool_id = current_tool
    ctrl = Qt.KeyboardModifier.ControlModifier

    assert controller.can_select_item(
        EllipseItem(QRectF(0, 0, 20, 20), QPen(QColor("red"), 4)), ctrl
    )
    assert controller.can_select_item(_text_item(), ctrl)
    assert controller.can_select_item(_stroke(), ctrl)


def test_plain_click_keeps_tool_filter_and_ctrl_capability_is_host_gated(qapp):
    scene = QGraphicsScene()
    controller = SmartEditController(scene)
    controller.current_tool_id = "text"
    ellipse = EllipseItem(QRectF(0, 0, 20, 20), QPen(QColor("red"), 4))

    assert not controller.can_select_item(ellipse, Qt.KeyboardModifier.NoModifier)
    assert not controller.can_select_item(ellipse, Qt.KeyboardModifier.ControlModifier)

    controller.cross_tool_select_enabled = True
    assert controller.can_select_item(ellipse, Qt.KeyboardModifier.ControlModifier)


def test_disabled_cross_tool_capability_preserves_legacy_ctrl_stroke_selection(qapp):
    controller = SmartEditController(QGraphicsScene())
    controller.current_tool_id = "text"
    ctrl = Qt.KeyboardModifier.ControlModifier

    assert controller.cross_tool_select_enabled is False
    assert controller.can_select_item(_stroke(), ctrl)
    assert not controller.can_select_item(
        EllipseItem(QRectF(0, 0, 20, 20), QPen(QColor("red"), 4)),
        ctrl,
    )


def test_cross_tool_state_uses_exact_item_owner(qapp):
    scene = QGraphicsScene()
    controller = SmartEditController(scene)
    controller.cross_tool_select_enabled = True
    controller.current_tool_id = "rect"
    ellipse = EllipseItem(QRectF(0, 0, 20, 20), QPen(QColor("red"), 4))
    scene.addItem(ellipse)
    controller.select_item(ellipse)

    assert controller.get_item_tool_id(ellipse) == "ellipse"
    assert controller.is_cross_tool_selection()


def test_plain_click_skips_top_text_to_select_compatible_shape_below(qapp):
    scene = QGraphicsScene()
    rect = RectItem(QRectF(0, 0, 80, 40), QPen(QColor("red"), 6))
    text = _text_item("overlap")
    text.setZValue(30)
    scene.addItem(rect)
    scene.addItem(text)

    controller = SmartEditController(scene)
    controller.current_tool_id = "rect"
    handled = controller.handle_press(
        QPointF(2, 2), QPointF(2, 2), Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier
    )

    assert handled
    assert controller.selected_item is rect


def test_ctrl_click_still_selects_topmost_editable_item(qapp):
    scene = QGraphicsScene()
    rect = RectItem(QRectF(0, 0, 80, 40), QPen(QColor("red"), 6))
    text = _text_item("overlap")
    text.setZValue(30)
    scene.addItem(rect)
    scene.addItem(text)

    controller = SmartEditController(scene)
    controller.cross_tool_select_enabled = True
    controller.current_tool_id = "rect"
    handled = controller.handle_press(
        QPointF(2, 2), QPointF(2, 2), Qt.MouseButton.LeftButton, Qt.KeyboardModifier.ControlModifier
    )

    assert handled
    assert controller.selected_item is text


def test_highlighter_panel_projects_logical_width(qapp):
    panel = PaintSettingsPanel()
    try:
        highlighter = _stroke(highlighter=True, width=15)
        panel.set_state_from_item(highlighter)
        assert highlighter.get_stroke_width() == 5
        assert panel.size_spin.value() == 5
    finally:
        panel.deleteLater()


def test_toolbar_session_reset_clears_temporary_edit_state(qapp):
    toolbar = Toolbar()
    try:
        toolbar.set_temporary_edit_active(True)
        assert toolbar.temporary_edit_active
        toolbar.reset_session_state()
        assert not toolbar.temporary_edit_active
    finally:
        toolbar.deleteLater()


def test_canvas_cross_tool_capability_is_explicit_and_host_scoped(qapp):
    default_scene, default_view = _canvas()
    enabled_scene, enabled_view = _canvas(cross_tool_select=True)
    try:
        assert not default_view.smart_edit_controller.cross_tool_select_enabled
        assert enabled_view.smart_edit_controller.cross_tool_select_enabled
    finally:
        default_view.close()
        enabled_view.close()
        default_scene.deleteLater()
        enabled_scene.deleteLater()


def test_cross_tool_style_helper_is_tristate_and_scales_logical_highlighter_width(qapp):
    scene, view = _canvas(cross_tool_select=True)
    try:
        scene.activate_tool("rect")
        highlighter = _stroke(highlighter=True, width=15)
        scene.addItem(highlighter)
        view.smart_edit_controller.select_item(highlighter)

        assert view.apply_cross_tool_selection_style(width=7) is True
        assert highlighter.get_stroke_width() == pytest.approx(7)
        assert highlighter.pen().widthF() == pytest.approx(21)

        view.smart_edit_controller.clear_selection()
        assert view.apply_cross_tool_selection_style(width=9) is None

        text = _text_item()
        scene.addItem(text)
        view.smart_edit_controller.select_item(text)
        assert view.apply_cross_tool_selection_style(width=9) is False
    finally:
        view.close()
        scene.deleteLater()


def test_cross_tool_color_preserves_target_alpha_and_opacity_is_direct(qapp):
    scene, view = _canvas(cross_tool_select=True)
    try:
        scene.activate_tool("text")
        color = QColor("red")
        color.setAlpha(91)
        rect = RectItem(QRectF(0, 0, 40, 20), QPen(color, 4))
        scene.addItem(rect)
        view.smart_edit_controller.select_item(rect)

        assert view.apply_cross_tool_selection_style(color=QColor("blue")) is True
        assert rect.pen().color().name() == QColor("blue").name()
        assert rect.pen().color().alpha() == 91
        assert view.apply_cross_tool_selection_style(opacity=0.35) is True
        assert rect.get_visual_opacity() == pytest.approx(0.35, abs=0.01)
    finally:
        view.close()
        scene.deleteLater()


def test_cross_selection_sync_does_not_overwrite_active_tool_defaults(qapp):
    host = QWidget()
    host.toolbar = Toolbar(host)
    scene, view = _canvas(cross_tool_select=True, parent=host)
    try:
        scene.activate_tool("rect")
        original_width = scene.tool_controller.ctx.stroke_width
        ellipse = EllipseItem(QRectF(0, 0, 30, 20), QPen(QColor("green"), 17))
        scene.addItem(ellipse)
        view.smart_edit_controller.current_tool_id = "rect"
        view.smart_edit_controller.select_item(ellipse)

        assert host.toolbar.temporary_edit_active
        assert scene.tool_controller.ctx.stroke_width == original_width
        assert host.toolbar.shape_panel.size_spin.value() == 17
    finally:
        view.close()
        host.close()
        scene.deleteLater()


def test_temporary_text_and_shape_handlers_emit_without_persisting(qapp, monkeypatch):
    toolbar = Toolbar()
    writes = []
    font_saves = []
    background_saves = []

    class Manager:
        def update_settings(self, tool_id, **kwargs):
            writes.append((tool_id, kwargs))

    monkeypatch.setattr("settings.get_tool_settings_manager", lambda: Manager())
    monkeypatch.setattr(
        TextSettingsPanel, "save_font_to_config", staticmethod(lambda font: font_saves.append(font))
    )
    monkeypatch.setattr(
        TextSettingsPanel,
        "save_background_to_config",
        staticmethod(lambda *args: background_saves.append(args)),
    )
    emitted = {"font": 0, "background": 0, "arrow": 0, "line": 0}
    toolbar.text_font_changed.connect(lambda _font: emitted.__setitem__("font", emitted["font"] + 1))
    toolbar.text_background_changed.connect(
        lambda *_args: emitted.__setitem__("background", emitted["background"] + 1)
    )
    toolbar.arrow_style_changed.connect(
        lambda _style: emitted.__setitem__("arrow", emitted["arrow"] + 1)
    )
    toolbar.line_style_changed.connect(
        lambda _style: emitted.__setitem__("line", emitted["line"] + 1)
    )
    try:
        toolbar.current_tool = "rect"
        toolbar.set_temporary_edit_active(True)
        toolbar._on_text_font_changed(QFont("Arial", 18))
        toolbar._on_text_background_changed(True, QColor("yellow"), 180)
        toolbar._on_arrow_style_changed("double")
        toolbar._on_line_style_changed("dashed")

        assert emitted == {"font": 1, "background": 1, "arrow": 1, "line": 1}
        assert writes == []
        assert font_saves == []
        assert background_saves == []
    finally:
        toolbar.deleteLater()


def test_temporary_creation_only_controls_are_consumed(qapp):
    toolbar = Toolbar()
    highlighter = []
    number = []
    toolbar.number_next_changed.connect(number.append)
    try:
        toolbar.set_temporary_edit_active(True)
        toolbar._on_highlighter_mode_changed("rectangle")
        toolbar._on_number_next_changed(8)
        assert not toolbar.paint_panel.mode_widget.isEnabled()
        assert not toolbar.number_panel.next_up_btn.isEnabled()
        assert highlighter == []
        assert number == []
    finally:
        toolbar.deleteLater()


@pytest.mark.parametrize(
    ("modifiers", "expected_width", "expected_opacity"),
    [
        (Qt.KeyboardModifier.NoModifier, 6, 128 / 255),
        (Qt.KeyboardModifier.ShiftModifier, 6, 128 / 255),
        (Qt.KeyboardModifier.ControlModifier, 5, (128 / 255) + 0.05),
    ],
)
def test_cross_tool_wheel_edits_target_without_touching_active_defaults(
    qapp, modifiers, expected_width, expected_opacity
):
    scene, view = _canvas(cross_tool_select=True)
    try:
        scene.activate_tool("number")
        original_default_width = scene.tool_controller.ctx.stroke_width
        target_color = QColor("red")
        target_color.setAlpha(128)
        rect = RectItem(QRectF(0, 0, 30, 20), QPen(target_color, 5))
        scene.addItem(rect)
        view.smart_edit_controller.current_tool_id = "number"
        view.smart_edit_controller.select_item(rect)
        event = QWheelEvent(
            QPointF(10, 10), QPointF(10, 10), QPoint(), QPoint(0, 120),
            Qt.MouseButton.NoButton, modifiers, Qt.ScrollPhase.ScrollUpdate, False,
        )

        view.wheelEvent(event)

        assert rect.get_stroke_width() == pytest.approx(expected_width)
        assert rect.get_visual_opacity() == pytest.approx(expected_opacity, abs=0.01)
        assert scene.tool_controller.ctx.stroke_width == original_default_width
    finally:
        view.close()
        scene.deleteLater()


def test_selection_clear_restores_current_tool_panel_and_defaults(qapp):
    host = QWidget()
    host.toolbar = Toolbar(host)
    scene, view = _canvas(cross_tool_select=True, parent=host)
    try:
        scene.activate_tool("rect")
        ctx = scene.tool_controller.ctx
        default_width = int(ctx.stroke_width)
        ellipse = EllipseItem(QRectF(0, 0, 30, 20), QPen(QColor("green"), 17))
        scene.addItem(ellipse)
        view.smart_edit_controller.current_tool_id = "rect"
        view.smart_edit_controller.select_item(ellipse)
        assert host.toolbar.shape_panel.size_spin.value() == 17

        view.smart_edit_controller.clear_selection()

        assert not host.toolbar.temporary_edit_active
        assert host.toolbar.shape_panel.size_spin.value() == default_width
        assert host.toolbar.shape_panel.isVisible() == host.toolbar.isVisible()
    finally:
        view.close()
        host.close()
        scene.deleteLater()


def test_cross_tool_hint_is_opt_in_and_only_on_editable_annotation_buttons(qapp):
    plain = Toolbar()
    hinted = Toolbar()
    try:
        phrase = "Ctrl+click any editable annotation to edit it temporarily"
        assert all(phrase not in button.toolTip() for button in plain.tool_buttons.values())
        hinted.enable_cross_tool_selection_hint()
        for tool_id in ("pen", "highlighter", "arrow", "number", "rect", "ellipse", "text"):
            assert phrase in hinted.tool_buttons[tool_id].toolTip()
        assert phrase not in hinted.mosaic_btn.toolTip()
        assert phrase not in hinted.eraser_btn.toolTip()
    finally:
        plain.deleteLater()
        hinted.deleteLater()


def test_cross_tool_hint_can_be_toggled_idempotently(qapp):
    toolbar = Toolbar()
    phrase = "Ctrl+click any editable annotation to edit it temporarily"
    button = toolbar.tool_buttons["text"]
    base = button.toolTip()
    try:
        toolbar.set_cross_tool_selection_hint_enabled(True)
        toolbar.set_cross_tool_selection_hint_enabled(True)
        assert button.toolTip().splitlines().count(phrase) == 1

        toolbar.set_cross_tool_selection_hint_enabled(False)
        toolbar.set_cross_tool_selection_hint_enabled(False)
        assert phrase not in button.toolTip()
        assert button.toolTip() == base
    finally:
        toolbar.deleteLater()


def test_cross_tool_hint_translations_load_from_runtime_resources(qapp):
    source = "Ctrl+click any editable annotation to edit it temporarily"
    expected = {
        "en": source,
        "zh": "按住 Ctrl 点击任意可编辑标注，可临时编辑",
        "ja": "Ctrlを押しながら編集可能な注釈をクリックして一時編集",
        "ko": "Ctrl 키를 누른 채 편집 가능한 주석을 클릭하여 임시 편집",
    }
    translations = Path(__file__).parents[1] / "translations"
    for language, translated in expected.items():
        translator = QTranslator()
        assert translator.load(str(translations / f"app_{language}.qm"))
        assert translator.translate("Toolbar", source) == translated


def test_view_owns_press_and_drag_when_compatible_item_is_below_top_text(qapp):
    scene, view = _canvas(cross_tool_select=True)
    try:
        scene.selection_model.initialize_confirmed_rect(QRectF(0, 0, 100, 80))
        scene.activate_tool("rect")
        rect = RectItem(QRectF(0, 0, 60, 30), QPen(QColor("red"), 6))
        text = _text_item("overlap")
        scene.addItem(rect)
        scene.addItem(text)
        start = rect.pos()

        view.mousePressEvent(QMouseEvent(
            QEvent.Type.MouseButtonPress,
            QPointF(2, 2), QPointF(2, 2),
            Qt.MouseButton.LeftButton, Qt.MouseButton.LeftButton,
            Qt.KeyboardModifier.NoModifier,
        ))
        view.mouseMoveEvent(QMouseEvent(
            QEvent.Type.MouseMove,
            QPointF(18, 8), QPointF(18, 8),
            Qt.MouseButton.NoButton, Qt.MouseButton.LeftButton,
            Qt.KeyboardModifier.NoModifier,
        ))
        view.mouseReleaseEvent(QMouseEvent(
            QEvent.Type.MouseButtonRelease,
            QPointF(18, 8), QPointF(18, 8),
            Qt.MouseButton.LeftButton, Qt.MouseButton.NoButton,
            Qt.KeyboardModifier.NoModifier,
        ))

        assert view.smart_edit_controller.selected_item is rect
        moved = rect.pos()
        assert moved != start
        assert text.pos() == QPointF(0, 0)
        assert not view._manual_item_drag_active
        assert not view.smart_edit_controller.is_dragging
        assert scene.undo_stack.canUndo()
        scene.undo_stack.undo()
        assert rect.pos() == start
        scene.undo_stack.redo()
        assert rect.pos() == moved
    finally:
        view.close()
        scene.deleteLater()


def test_manual_lower_item_drag_finishes_on_tool_switch_with_one_undo(qapp):
    scene, view = _canvas(cross_tool_select=True)
    try:
        scene.selection_model.initialize_confirmed_rect(QRectF(0, 0, 100, 80))
        scene.activate_tool("rect")
        rect = RectItem(QRectF(0, 0, 60, 30), QPen(QColor("red"), 6))
        scene.addItem(rect)
        scene.addItem(_text_item("overlap"))
        start = rect.pos()
        view.mousePressEvent(QMouseEvent(
            QEvent.Type.MouseButtonPress, QPointF(2, 2), QPointF(2, 2),
            Qt.MouseButton.LeftButton, Qt.MouseButton.LeftButton,
            Qt.KeyboardModifier.NoModifier,
        ))
        view.mouseMoveEvent(QMouseEvent(
            QEvent.Type.MouseMove, QPointF(18, 8), QPointF(18, 8),
            Qt.MouseButton.NoButton, Qt.MouseButton.LeftButton,
            Qt.KeyboardModifier.NoModifier,
        ))
        moved = rect.pos()
        scene.activate_tool("ellipse")

        assert moved != start
        assert not view._manual_item_drag_active
        assert not view.smart_edit_controller.is_dragging
        assert view.smart_edit_controller.selected_item is None
        assert scene.undo_stack.canUndo()
        scene.undo_stack.undo()
        assert rect.pos() == start
        assert not scene.undo_stack.canUndo()
    finally:
        view.close()
        scene.deleteLater()


def test_topmost_compatible_item_keeps_native_dispatch_path(qapp):
    scene, view = _canvas(cross_tool_select=True)
    try:
        scene.selection_model.initialize_confirmed_rect(QRectF(0, 0, 100, 80))
        scene.activate_tool("rect")
        rect = RectItem(QRectF(0, 0, 60, 30), QPen(QColor("red"), 6))
        scene.addItem(rect)
        view.mousePressEvent(QMouseEvent(
            QEvent.Type.MouseButtonPress, QPointF(2, 2), QPointF(2, 2),
            Qt.MouseButton.LeftButton, Qt.MouseButton.LeftButton,
            Qt.KeyboardModifier.NoModifier,
        ))
        assert view.smart_edit_controller.selected_item is rect
        assert not view._manual_item_drag_active
    finally:
        view.close()
        scene.deleteLater()


def test_screenshot_tool_activation_reloads_full_target_panel_after_temporary_projection(
    qapp, monkeypatch
):
    monkeypatch.setattr("PySide6.QtCore.QTimer.singleShot", lambda *_args: None)
    host = QWidget()
    host.toolbar = Toolbar(host)
    scene, view = _canvas(cross_tool_select=True, parent=host)
    host.scene = scene
    host.view = view
    host.magnifier_overlay = None
    class Manager:
        values = {
            "text": {
                "font_family": "Arial",
                "font_size": 13,
                "font_bold": False,
                "font_italic": False,
                "font_underline": False,
                "color": "#010203",
                "background_enabled": False,
                "background_color": "#ffffff",
                "background_opacity": 220,
                "opacity": 1.0,
            },
            "arrow": {"arrow_style": "single", "stroke_width": 9, "opacity": 1.0},
            "pen": {"line_style": "solid", "stroke_width": 12, "opacity": 1.0},
        }

        def get_tool_settings(self, tool_id):
            return self.values.get(tool_id, {})

    monkeypatch.setattr("settings.get_tool_settings_manager", lambda: Manager())
    try:
        host.toolbar.text_panel.size_spin.setValue(31)
        host.toolbar.text_panel.background_enabled = True
        host.toolbar.arrow_panel.arrow_style = "double"
        host.toolbar.paint_panel.line_style = "dashed"
        ScreenshotWindow.on_tool_changed(host, "text")
        assert host.toolbar.text_panel.size_spin.value() == 13
        assert not host.toolbar.text_panel.background_enabled
        ScreenshotWindow.on_tool_changed(host, "arrow")
        assert host.toolbar.arrow_panel.arrow_style == "single"
        ScreenshotWindow.on_tool_changed(host, "pen")
        assert host.toolbar.paint_panel.line_style == "solid"
    finally:
        view.close()
        host.close()
        scene.deleteLater()
