# -*- coding: utf-8 -*-
"""
聚光灯测试

聚光灯的语义是"整个场景一张黑色幕布，每个聚光灯是幕布上的一个孔"。最容易被顺手破坏
的几条单独守着：重叠的孔不能被压暗两次；孔的增删要让整张幕布重画；孔虽然是 RectItem，
却不能被矩形工具当普通矩形选走、也不能改写矩形工具的设置；钉图克隆要带上幕布的暗度。
"""
import pytest

from PySide6.QtCore import QPoint, QPointF, QRectF, QSize, Qt
from PySide6.QtGui import QBrush, QColor, QImage, QPainter, QPainterPath, QPen
from PySide6.QtWidgets import QApplication, QWidget

from canvas.items import MosaicItem, RectItem, SpotlightCurtain, SpotlightItem
from canvas.scene import CanvasScene
from core.export import ExportService
from settings import get_tool_settings_manager

SIZE = 64
GRAY = QColor(200, 200, 200)


@pytest.fixture(autouse=True)
def spotlight_settings():
    """50% 暗度的幕布；测试写进共享配置的聚光灯/矩形设置，用完还原。"""
    manager = get_tool_settings_manager()
    before = {
        tool: manager.get_tool_settings(tool).to_dict() for tool in ("spotlight", "rect")
    }
    manager.update_settings("spotlight", opacity=0.5)
    yield manager
    for tool, values in before.items():
        manager.update_settings(tool, **values)


def _scene():
    image = QImage(SIZE, SIZE, QImage.Format.Format_ARGB32)
    image.fill(GRAY)
    scene = CanvasScene(image, QRectF(0, 0, SIZE, SIZE), enable_mosaic=True)
    scene.selection_model.initialize_confirmed_rect(QRectF(0, 0, SIZE, SIZE))
    return scene


def _draw_spotlight(scene, start, end):
    scene.activate_tool("spotlight")
    controller = scene.tool_controller
    controller.on_press(QPointF(*start), Qt.MouseButton.LeftButton)
    controller.on_move(QPointF(*end))
    controller.on_release(QPointF(*end))


def _spotlights(scene):
    return [item for item in scene.items() if isinstance(item, SpotlightItem)]


def _curtains(scene):
    return [item for item in scene.items() if isinstance(item, SpotlightCurtain)]


def _export(scene, rect=QRectF(0, 0, SIZE, SIZE)):
    return ExportService(scene).export(rect)


def _assert_color(image, x, y, expected, tolerance=2):
    actual = image.pixelColor(x, y)
    assert max(
        abs(actual.red() - expected.red()),
        abs(actual.green() - expected.green()),
        abs(actual.blue() - expected.blue()),
    ) <= tolerance, ((x, y), actual.getRgb(), expected.getRgb())


def _dimmed(color, darkness=0.5):
    """半透明黑色叠在 color 上的结果"""
    return QColor(*(round(channel * (1 - darkness)) for channel in color.getRgb()[:3]))


def test_drawing_a_spotlight_is_undoable_and_tiny_drags_are_dropped(qapp):
    scene = _scene()
    _draw_spotlight(scene, (10, 10), (14, 14))
    assert _spotlights(scene) == []

    _draw_spotlight(scene, (10, 10), (40, 30))
    [item] = _spotlights(scene)
    assert item.sceneBoundingRect().contains(QRectF(10, 10, 30, 20))

    scene.undo_stack.undo()
    assert _spotlights(scene) == []
    _assert_color(_export(scene), 4, 60, GRAY)


def test_overlapping_spotlights_share_one_curtain(qapp):
    """重叠的孔不能被压暗两次，孔外也只压暗一次"""
    scene = _scene()
    _draw_spotlight(scene, (8, 8), (40, 40))
    _draw_spotlight(scene, (24, 24), (56, 56))

    assert len(_spotlights(scene)) == 2
    assert len(_curtains(scene)) == 1

    image = _export(scene)
    _assert_color(image, 30, 30, GRAY)       # 两个孔的重叠处
    _assert_color(image, 12, 12, GRAY)       # 只在第一个孔里
    _assert_color(image, 50, 50, GRAY)       # 只在第二个孔里
    _assert_color(image, 4, 60, _dimmed(GRAY))


def test_curtain_dims_mosaic_but_not_annotations(qapp):
    """幕布压在马赛克之上、标注之下"""
    scene = _scene()
    red = QImage(8, 8, QImage.Format.Format_ARGB32)
    red.fill(QColor(255, 0, 0))
    path = QPainterPath()
    path.addRect(QRectF(0, 0, 16, 16))
    scene.addItem(MosaicItem(path, 10, 8, red, QRectF(0, 0, SIZE, SIZE), fill_mode=True))

    blue = RectItem(QRectF(48, 0, 16, 16), QPen(Qt.PenStyle.NoPen))
    blue.setBrush(QBrush(QColor(0, 0, 255)))
    scene.addItem(blue)

    _draw_spotlight(scene, (24, 24), (40, 40))

    image = _export(scene)
    _assert_color(image, 8, 8, _dimmed(QColor(255, 0, 0)))
    _assert_color(image, 56, 8, QColor(0, 0, 255))


def test_adding_or_removing_a_hole_repaints_the_whole_curtain(qapp):
    """一个孔都没有时幕布不画。只重画孔自己那一块的话，孔外那一大片会停在旧画面上"""
    scene = _scene()
    _draw_spotlight(scene, (20, 20), (40, 40))
    QApplication.processEvents()

    changed = []
    scene.changed.connect(changed.extend)
    for step in (scene.undo_stack.undo, scene.undo_stack.redo):
        changed.clear()
        step()
        QApplication.processEvents()
        dirty = QRectF()
        for rect in changed:
            dirty = dirty.united(rect)
        assert dirty.contains(scene.sceneRect()), step


def test_darkness_follows_the_selected_hole_and_the_tool_for_new_holes(qapp):
    scene = _scene()
    _draw_spotlight(scene, (20, 20), (40, 40))
    [curtain] = _curtains(scene)
    [item] = _spotlights(scene)
    assert curtain.opacity() == pytest.approx(0.5)

    # 选中的孔改透明度，改的就是整张幕布
    assert item.set_visual_opacity(0.25)
    assert curtain.opacity() == pytest.approx(0.25)
    assert item.get_visual_opacity() == pytest.approx(0.25)
    _assert_color(_export(scene), 4, 60, _dimmed(GRAY, 0.25))

    # 工具的透明度和其他工具一样只管下一次绘制；画下一个孔时，整张幕布跟着统一
    scene.update_style(opacity=0.8)
    assert curtain.opacity() == pytest.approx(0.25)
    _draw_spotlight(scene, (44, 44), (60, 60))
    assert curtain.opacity() == pytest.approx(0.8)


def test_spotlight_is_not_a_plain_rectangle_for_selection(qapp):
    from canvas.smart_edit_controller import SmartEditController

    scene = _scene()
    spotlight = SpotlightItem(QRectF(10, 10, 20, 20))
    rect = RectItem(QRectF(10, 10, 20, 20), QPen(QColor(255, 0, 0), 3))
    scene.addItem(spotlight)
    scene.addItem(rect)
    controller = SmartEditController(scene)
    no_modifier = Qt.KeyboardModifier.NoModifier

    assert controller.get_item_tool_id(spotlight) == "spotlight"
    for tool in ("rect", "ellipse", "highlighter", "cursor"):
        controller.set_tool(tool)
        assert not controller.can_select_item(spotlight, no_modifier), tool
        assert not controller.can_show_hover_cursor(spotlight), tool

    controller.set_tool("spotlight")
    assert controller.can_select_item(spotlight, no_modifier)
    assert controller.can_show_hover_cursor(spotlight)
    assert not controller.can_select_item(rect, no_modifier)


def test_selecting_a_spotlight_shows_only_darkness_and_keeps_the_curtain(
    qapp, spotlight_settings, monkeypatch
):
    """选中孔：面板只剩透明度；回填到工具的暗度不能反过来冲掉幕布，也不能改写矩形工具的设置"""
    from canvas.view import CanvasView
    from ui.toolbar import Toolbar

    spotlight_settings.update_settings("rect", line_style="dashed")
    scene = _scene()
    view = CanvasView(scene)
    toolbar = Toolbar()
    monkeypatch.setattr(view, "_get_active_toolbar", lambda: toolbar)
    try:
        _draw_spotlight(scene, (20, 20), (40, 40))
        [item] = _spotlights(scene)
        view.smart_edit_controller.clear_selection(suppress_block=True)   # 画完会自动选中
        item.set_visual_opacity(0.3)   # 幕布暗度和工具设置（0.5）不一致，比如钉图克隆过来的
        view.smart_edit_controller.select_item(item)

        panel = toolbar.paint_panel
        assert not panel.isHidden()
        assert toolbar.shape_panel.isHidden()
        assert panel.size_spin.isHidden()
        assert panel.mode_widget.isHidden()
        assert panel.line_style_combo.isHidden()
        assert panel.color_widget.isHidden()
        assert panel.opacity_spin.value() == 30
        assert item.get_visual_opacity() == pytest.approx(0.3)
        assert spotlight_settings.get_setting("rect", "line_style") == "dashed"

        # 回到荧光笔，面板的线宽和颜色控件要回来
        toolbar._show_panel_for_tool("highlighter")
        assert not panel.size_spin.isHidden()
        assert not panel.color_widget.isHidden()
    finally:
        toolbar.deleteLater()


def test_default_toolbar_folds_the_spotlight_button(qapp):
    from ui.toolbar import Toolbar

    toolbar = Toolbar()
    try:
        assert "spotlight" in toolbar._folded_keys
        toolbar.select_tool("spotlight")
        assert toolbar.current_tool == "spotlight"
        assert toolbar.spotlight_btn.isChecked()
    finally:
        toolbar.deleteLater()


def test_spotlight_clones_into_pin_with_its_curtain(qapp):
    from pin.pin_canvas import PinCanvas

    source = _scene()
    _draw_spotlight(source, (16, 16), (40, 40))
    [source_item] = _spotlights(source)
    source_item.set_visual_opacity(0.4)
    source_item.set_corner_radius(6)

    selection = QRectF(8, 0, 48, 64)
    drawing_items = source.get_drawing_items_in_rect(selection)
    assert drawing_items == [source_item]   # 幕布不是独立标注，不参与克隆

    parent = QWidget()
    parent._is_editing = False
    parent.toolbar = None
    pin = PinCanvas(parent, QSize(48, 64), source.background.image().copy(8, 0, 48, 64))
    pin.initialize_from_items(drawing_items, QPoint(8, 0))

    [cloned] = _spotlights(pin.scene)
    assert type(cloned) is SpotlightItem
    assert cloned.rect() == source_item.rect()
    assert cloned.pos() == QPointF(-8, 0)
    assert cloned.get_visual_opacity() == pytest.approx(0.4)
    assert cloned.get_corner_radius() == 6

    expected = _export(source, selection)
    rendered = QImage(48, 64, QImage.Format.Format_ARGB32)
    rendered.fill(Qt.GlobalColor.transparent)
    painter = QPainter(rendered)
    pin.render_to_painter(painter, QRectF(0, 0, 48, 64))
    painter.end()
    for x, y in ((2, 2), (20, 20), (40, 60), (30, 30), (9, 17)):
        _assert_color(rendered, x, y, expected.pixelColor(x, y))


def test_rounded_hole_follows_the_corner_radius(qapp):
    """孔继承了矩形的圆角手柄，挖出来的形状就得跟着圆角走，不然虚线框和孔对不上"""
    scene = _scene()
    _draw_spotlight(scene, (10, 10), (50, 50))
    [item] = _spotlights(scene)

    _assert_color(_export(scene), 11, 11, GRAY)
    item.set_corner_radius(12)
    image = _export(scene)
    _assert_color(image, 11, 11, _dimmed(GRAY))   # 圆角切掉的角落
    _assert_color(image, 30, 30, GRAY)
    assert not item.hole_path().contains(QPointF(11, 11))


def test_the_whole_hole_selects_the_spotlight_but_the_curtain_does_not(qapp):
    """孔没有描边，只认边太难选中；孔外的幕布是所有孔共用的，不属于任何一个孔"""
    from canvas.smart_edit_controller import SmartEditController

    scene = _scene()
    _draw_spotlight(scene, (16, 16), (48, 48))
    [item] = _spotlights(scene)
    controller = SmartEditController(scene)
    controller.cross_tool_select_enabled = True
    ctrl = Qt.KeyboardModifier.ControlModifier

    for name, pos, expected in (
        ("孔内", QPointF(32, 32), True),
        ("孔的边", QPointF(16, 32), True),
        ("孔外的幕布", QPointF(4, 4), False),
    ):
        controller.clear_selection(suppress_block=True)
        controller.set_tool("pen")
        controller.handle_press(pos, pos, Qt.MouseButton.LeftButton, ctrl)
        assert (controller.selected_item is item) is expected, name
