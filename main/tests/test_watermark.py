# -*- coding: utf-8 -*-
"""
水印测试

水印是场景里唯一的一层文字，范围始终是选区。守着几件容易被顺手破坏的事：只画与重绘区
相交的格子时不能漏画；八种位置各就各位、多行都画出来；铺上、去掉都能撤销且场景里最多一层；
选区动了水印跟上；时间按加水印那一刻展开、改文字也不变；导出带水印、给 OCR 的纯底图不带；
点选和橡皮擦穿过水印；钉图克隆后对上钉图的整张图；工具栏、样式弹层、输入框各自接对了线。
"""
from datetime import datetime

import pytest

from PySide6.QtCore import QPoint, QPointF, QRectF, QSize, Qt
from PySide6.QtGui import QColor, QImage, QPainter, QPen, QTextCursor
from PySide6.QtWidgets import QWidget

from canvas.items import RectItem, WatermarkItem, WatermarkStyle
from canvas.items.watermark_item import (
    ANCHORS, LAYOUT_TILE, apply_watermark, expand_text, paint_watermark, watermark_font, watermark_placements,
)
from canvas.scene import CanvasScene
from core.export import ExportService
from settings import get_tool_settings_manager
from tools.watermark import WatermarkTool

W, H = 320, 200
WHITE = QColor(255, 255, 255)
# 不透明的黑色：像素一比就知道画没画上
INK = WatermarkStyle(color="#000000", font_size=16, opacity=1.0)
NOW = datetime(2026, 10, 11, 9, 5, 7)


@pytest.fixture(autouse=True)
def watermark_settings():
    """测试会改水印样式、文字、自动加和时间格式，用完还原。"""
    manager = get_tool_settings_manager()
    style = manager.get_tool_settings("watermark").to_dict()
    saved = {key: manager.qsettings.value(f"app/{key}") for key in (
        "screenshot_watermark_text", "screenshot_watermark_auto", "screenshot_watermark_time_format")}
    manager.update_settings("watermark", **vars(INK))
    manager.set_screenshot_watermark_text("WATERMARK")
    yield manager
    manager.update_settings("watermark", **style)
    for key, value in saved.items():
        if value is None:
            manager.qsettings.remove(f"app/{key}")
        else:
            manager.qsettings.setValue(f"app/{key}", value)


def _scene(selection=QRectF(40, 20, 240, 160)):
    image = QImage(W, H, QImage.Format.Format_ARGB32)
    image.fill(WHITE)
    scene = CanvasScene(image, QRectF(0, 0, W, H), enable_mosaic=True)
    scene.selection_model.initialize_confirmed_rect(selection)
    return scene


def _blank(w=W, h=H):
    image = QImage(w, h, QImage.Format.Format_ARGB32)
    image.fill(WHITE)
    return image


def _inked(image, rect=None):
    """rect 里不是白色的像素个数"""
    rect = (rect or QRectF(image.rect())).toRect()
    return sum(
        1
        for x in range(rect.left(), rect.right() + 1)
        for y in range(rect.top(), rect.bottom() + 1)
        if image.pixelColor(x, y) != WHITE
    )


def _inked_rows(image):
    return [y for y in range(image.height()) if any(image.pixelColor(x, y) != WHITE for x in range(image.width()))]


def _paint(rect, style=INK, text="WATERMARK", exposed=None, clip=None, image=None):
    image = image or _blank()
    painter = QPainter(image)
    if clip is not None:
        painter.setClipRect(clip)
    paint_watermark(painter, rect, text, style, exposed)
    painter.end()
    return image


# ---------------------------------------------------------------------------
# 文字、样式、绘制
# ---------------------------------------------------------------------------

def test_time_tokens_expand_to_the_given_moment():
    assert expand_text("ACME $yyyy-MM-dd$ $HH:mm$", NOW) == "ACME 2026-10-11 09:05"
    assert expand_text("  line1\n$HH:mm:ss$  ", NOW) == "line1\n09:05:07"


def test_unpaired_dollar_signs_stay_as_typed():
    assert expand_text("cost $5", NOW) == "cost $5"
    assert expand_text("$$", NOW) == "$"


def test_every_time_format_in_the_menu_expands():
    from ui.watermark_input_dialog import TIME_FORMATS, time_token

    for name, fmt, _description in TIME_FORMATS:
        text = expand_text(time_token(fmt), NOW)
        assert text and "$" not in text and text != fmt, name
    assert expand_text("$yyyyMMddHHmmss$", NOW) == "20261011090507"


def test_style_is_clamped_to_valid_values():
    style = WatermarkStyle(color="nope", font_size=1000, opacity=3, layout="spiral",
                           spacing=-5, angle=400, font_family=None).normalized()
    assert style == WatermarkStyle(color=WatermarkStyle.color, font_size=96, opacity=1.0,
                                   spacing=0, angle=90, font_family="")


def test_font_family_follows_the_style():
    assert watermark_font(INK.with_changes(font_family="Arial")).family() == "Arial"
    assert watermark_font(INK).pixelSize() == INK.font_size


@pytest.mark.parametrize("text", ["WATERMARK", "LINE ONE\nTWO"])
def test_drawing_only_the_exposed_tiles_matches_drawing_everything(qapp, text):
    """局部重绘只画相交的格子；把选区切成小块各画一遍，拼起来必须和整块画一次一样"""
    rect = QRectF(13, 7, 290, 181)
    whole = _paint(rect, text=text)
    pieces = _blank()
    step = 37
    for x in range(0, W, step):
        for y in range(0, H, step):
            piece = QRectF(x, y, step, step)
            _paint(rect, text=text, exposed=piece, clip=piece, image=pieces)
    assert pieces == whole
    assert _inked(whole) > 0


def test_a_small_repaint_draws_only_a_few_tiles(qapp):
    rect = QRectF(0, 0, 2560, 1440)
    style = WatermarkStyle(font_size=24)
    everything = watermark_placements(rect, "2026-10-11 14:30", style)
    # 画笔拖动一帧大约重画这么大一块
    nearby = watermark_placements(rect, "2026-10-11 14:30", style, QRectF(1000, 600, 400, 300))
    assert 0 < len(nearby) <= len(everything) // 4


def test_spacing_thins_out_the_tiles(qapp):
    rect = QRectF(0, 0, 1600, 900)
    counts = [len(watermark_placements(rect, "MARK", INK.with_changes(spacing=s))) for s in (0, 40, 100)]
    assert counts[0] > counts[1] > counts[2] > 0


def test_angle_only_rotates_tiles(qapp):
    rect = QRectF(0, 0, 400, 300)
    assert {angle for _c, angle in watermark_placements(rect, "MARK", INK.with_changes(angle=15))} == {15}
    assert watermark_placements(rect, "MARK", INK.with_changes(layout="center", angle=15))[0][1] == 0


def test_tiles_stay_inside_the_rect(qapp):
    rect = QRectF(60, 40, 120, 80)
    image = _paint(rect)
    assert _inked(image, rect) > 0
    assert _inked(image) == _inked(image, rect)


@pytest.mark.parametrize("layout", sorted(ANCHORS))
def test_each_position_lands_in_its_part_of_the_selection(qapp, layout):
    rect = QRectF(20, 10, 280, 180)
    image = _paint(rect, style=INK.with_changes(layout=layout), text="MARK")
    assert _inked(image) == _inked(image, rect) > 0
    [(center, _angle)] = watermark_placements(rect, "MARK", INK.with_changes(layout=layout))
    ax, ay = ANCHORS[layout]
    for value, low, size, anchor in ((center.x(), rect.left(), rect.width(), ax),
                                     (center.y(), rect.top(), rect.height(), ay)):
        third = (value - low) / size
        assert {0.0: third < 1 / 3, 0.5: 1 / 3 <= third <= 2 / 3, 1.0: third > 2 / 3}[anchor], (layout, third)


def test_every_line_of_a_multiline_watermark_is_drawn(qapp):
    rect = QRectF(0, 0, W, H)
    style = INK.with_changes(layout="center")
    one = _inked_rows(_paint(rect, style=style, text="LINE"))
    two = _inked_rows(_paint(rect, style=style, text="LINE\nLINE"))
    assert max(two) - min(two) > 1.6 * (max(one) - min(one))


def test_a_selection_narrower_than_the_text_keeps_the_text_centered(qapp):
    rect = QRectF(100, 100, 20, 20)
    [(center, _angle)] = watermark_placements(rect, "A VERY LONG WATERMARK", INK.with_changes(layout="top_left"))
    assert center == rect.center()


# ---------------------------------------------------------------------------
# 场景里的水印
# ---------------------------------------------------------------------------

def test_watermark_follows_the_selection_and_exports_with_it(qapp):
    scene = _scene()
    item = WatermarkTool.add(scene)
    assert item.text() == "WATERMARK"
    assert item.rect() == QRectF(40, 20, 240, 160)

    scene.selection_model.set_rect(QRectF(10, 10, 200, 150))
    assert item.rect() == QRectF(10, 10, 200, 150)

    selection = scene.selection_model.rect()
    exporter = ExportService(scene)
    assert _inked(exporter.export(selection)) > 0
    # 钉图和 OCR 拿的纯底图不带水印
    assert _inked(exporter.export_base_image_only(selection)) == 0


def test_adding_and_removing_are_undoable_and_there_is_only_one(qapp):
    scene = _scene()
    item = WatermarkTool.add(scene)
    assert WatermarkTool.add(scene) is item
    assert [i for i in scene.items() if isinstance(i, WatermarkItem)] == [item]

    scene.undo_stack.undo()
    assert WatermarkItem.find(scene) is None
    scene.undo_stack.redo()
    assert WatermarkItem.find(scene) is item

    assert WatermarkTool.remove(scene)
    assert WatermarkItem.find(scene) is None
    assert not WatermarkTool.remove(scene)
    scene.undo_stack.undo()
    assert WatermarkItem.find(scene) is item


def test_redo_puts_the_watermark_on_the_current_selection(qapp):
    """撤销期间选区变了，重做回来的水印要对上现在的选区，而不是当初的"""
    scene = _scene()
    item = WatermarkTool.add(scene)
    scene.undo_stack.undo()
    scene.selection_model.set_rect(QRectF(0, 0, 100, 100))
    scene.undo_stack.redo()
    assert item.rect() == QRectF(0, 0, 100, 100)


def test_auto_watermark_is_not_on_the_undo_stack(qapp):
    scene = _scene()
    WatermarkTool.add(scene, undoable=False)
    assert WatermarkItem.find(scene) is not None
    assert not scene.undo_stack.canUndo()


def test_style_changes_apply_to_the_watermark_and_become_the_default(qapp, watermark_settings):
    scene = _scene()
    item = WatermarkTool.add(scene)
    WatermarkTool.change_style(scene, font_size=40, layout="bottom_right", spacing=70, angle=10)
    assert (item.style().font_size, item.style().layout, item.style().spacing, item.style().angle) == \
        (40, "bottom_right", 70, 10)
    assert WatermarkTool.load_style(watermark_settings) == item.style()
    # 样式不进撤销栈：撤销只撤掉铺上水印这一步
    scene.undo_stack.undo()
    assert WatermarkItem.find(scene) is None


def test_editing_the_text_keeps_the_time_the_watermark_was_added(qapp, watermark_settings):
    scene = _scene()
    item = WatermarkTool.add(scene)
    WatermarkTool.change_text(scene, "ACME $yyyy-MM-dd HH:mm:ss.zzz$")
    assert item.template() == "ACME $yyyy-MM-dd HH:mm:ss.zzz$"
    assert item.text() == expand_text(item.template(), item.created_at())
    assert watermark_settings.get_screenshot_watermark_text() == "ACME $yyyy-MM-dd HH:mm:ss.zzz$"


def test_clicks_and_the_eraser_pass_through_the_watermark(qapp):
    from canvas.smart_edit_controller import SmartEditController

    scene = _scene()
    rect = RectItem(QRectF(100, 60, 80, 60), QPen(QColor("red"), 4))
    scene.addItem(rect)
    WatermarkTool.add(scene)
    point = QPointF(100, 90)
    assert not any(isinstance(i, WatermarkItem) for i in scene.items(point))

    controller = SmartEditController(scene)
    controller.set_tool("rect")
    controller.handle_press(point, point, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier)
    assert controller.selected_item is rect

    scene.activate_tool("eraser")
    tools = scene.tool_controller
    tools.on_press(QPointF(45, 25), Qt.MouseButton.LeftButton)
    tools.on_move(QPointF(270, 170))
    tools.on_release(QPointF(270, 170))
    assert WatermarkItem.find(scene) is not None


def test_watermark_tool_does_not_touch_the_drawing_context(qapp):
    """选中水印工具不能把画笔的颜色、粗细换成水印的"""
    scene = _scene()
    scene.activate_tool("pen")
    ctx = scene.tool_controller.ctx
    before = (QColor(ctx.color), ctx.stroke_width, ctx.opacity)
    scene.activate_tool("watermark")
    scene.activate_tool("pen")
    assert (QColor(ctx.color), ctx.stroke_width, ctx.opacity) == before


def test_watermark_clones_into_pin_and_covers_the_whole_pin(qapp, watermark_settings):
    from pin.pin_canvas import PinCanvas

    watermark_settings.set_screenshot_watermark_text("PIN $HH:mm:ss.zzz$")
    selection = QRectF(40, 20, 240, 160)
    source = _scene(selection)
    source_item = WatermarkTool.add(source)
    drawing_items = source.get_drawing_items_in_rect(selection)
    assert drawing_items == [source_item]

    parent = QWidget()
    parent._is_editing = False
    parent.toolbar = None
    pin = PinCanvas(parent, QSize(240, 160), source.background.image().copy(selection.toRect()))
    pin.initialize_from_items(drawing_items, QPoint(40, 20))

    cloned = WatermarkItem.find(pin.scene)
    assert cloned is not None and cloned is not source_item
    # 毫秒都一样：钉图沿用加水印的那一刻，不是克隆的那一刻
    assert cloned.text() == source_item.text()
    assert cloned.template() == source_item.template()
    assert cloned.style() == source_item.style()
    assert cloned.mapRectToScene(cloned.rect()) == QRectF(0, 0, 240, 160)

    # 钉图窗口画在预乘格式的后备缓冲上；非预乘的目标图里旋转文字的落点会不一样
    expected = ExportService(source).export(selection)
    rendered = QImage(240, 160, QImage.Format.Format_ARGB32_Premultiplied)
    rendered.fill(Qt.GlobalColor.transparent)
    painter = QPainter(rendered)
    pin.render_to_painter(painter, QRectF(0, 0, 240, 160))
    painter.end()
    assert _inked(expected) > 0
    assert rendered == expected


def test_quick_capture_gets_the_watermark_only_when_auto_is_on(qapp, watermark_settings):
    image = _blank(200, 120)
    watermark_settings.set_screenshot_watermark_auto(False)
    assert WatermarkTool.apply_to_image(image, watermark_settings) is image

    watermark_settings.set_screenshot_watermark_auto(True)
    marked = WatermarkTool.apply_to_image(image, watermark_settings)
    assert _inked(marked) > 0
    assert _inked(image) == 0   # 原图不动

    watermark_settings.set_screenshot_watermark_text("")
    assert WatermarkTool.apply_to_image(image, watermark_settings) is image


def test_apply_watermark_matches_the_canvas_export(qapp):
    """快速截图直接画到图上，和普通截图从画布导出的必须是同一个样子"""
    scene = _scene(QRectF(0, 0, W, H))
    WatermarkTool.add(scene)
    exported = ExportService(scene).export(QRectF(0, 0, W, H))
    item = WatermarkItem.find(scene)
    applied = apply_watermark(_blank(), item.text(), item.style())
    assert applied.convertToFormat(exported.format()) == exported


# ---------------------------------------------------------------------------
# 工具栏、面板、样式弹层、输入框
# ---------------------------------------------------------------------------

def test_toolbar_button_adds_then_removes(qapp):
    from ui.toolbar import Toolbar

    toolbar = Toolbar()
    toggles, styles = [], []
    toolbar.watermark_toggled.connect(toggles.append)
    toolbar.watermark_style_changed.connect(lambda key, value: styles.append((key, value)))
    assert toolbar.watermark_panel is None   # 用到才建

    toolbar.watermark_btn.click()
    panel = toolbar.watermark_panel
    assert toolbar.current_tool == "watermark" and toggles == [True]
    assert panel.isVisible()
    assert panel.text_field.template() == "WATERMARK"

    panel.layout_combo.setCurrentIndex(panel.layout_combo.findData("bottom_right"))
    panel.font_combo.setCurrentIndex(panel.font_combo.findData("Arial"))
    assert styles == [("layout", "bottom_right"), ("font_family", "Arial")]

    # 切到别的工具，水印还在；再点水印按钮只是重新打开面板
    toolbar.pen_btn.click()
    assert not panel.isVisible()
    toolbar.watermark_btn.click()
    assert toolbar.current_tool == "watermark" and toggles == [True, True]

    toolbar.watermark_btn.click()
    assert toolbar.current_tool is None and toggles == [True, True, False]
    assert not toolbar.watermark_btn.isChecked()
    assert not panel.isVisible()


def test_style_popup_sliders_and_colors_report_their_fields(qapp):
    from ui.watermark_settings_panel import WatermarkSettingsPanel

    panel = WatermarkSettingsPanel()
    changes = []
    panel.style_changed.connect(lambda key, value: changes.append((key, value)))
    panel.load_style(INK)
    panel.show_style_popup()
    popup = panel._style_popup
    assert popup.isVisible()
    assert popup._sliders["font_size"].value() == INK.font_size
    assert popup._sliders["opacity"].value() == 100

    popup._sliders["opacity"].setValue(30)
    popup._sliders["spacing"].setValue(55)
    popup._sliders["angle"].setValue(-45)
    popup._on_preset("#FF0000")
    assert changes == [("opacity", 0.3), ("spacing", 55), ("angle", -45), ("color", "#ff0000")]

    # 角度只对平铺起作用，换成单个位置后那一行变灰
    panel.layout_combo.setCurrentIndex(panel.layout_combo.findData("center"))
    assert not popup._sliders["angle"].isEnabled()
    panel.layout_combo.setCurrentIndex(panel.layout_combo.findData(LAYOUT_TILE))
    assert popup._sliders["angle"].isEnabled()
    popup.hide()


def test_text_field_opens_the_editor_and_clears(qapp, monkeypatch):
    import ui.watermark_input_dialog as dialog_module
    from ui.toolbar import Toolbar

    toolbar = Toolbar()
    texts = []
    toolbar.watermark_text_changed.connect(texts.append)
    toolbar.watermark_btn.click()
    panel = toolbar.watermark_panel

    monkeypatch.setattr(dialog_module, "edit_watermark_text", lambda anchor, manager: "ACME\n$HH:mm$")
    panel.text_field.clicked.emit()
    assert texts == ["ACME\n$HH:mm$"]
    assert panel.text_field.template() == "ACME\n$HH:mm$"

    # 取消输入框什么都不改
    monkeypatch.setattr(dialog_module, "edit_watermark_text", lambda anchor, manager: None)
    panel.text_field.clicked.emit()
    assert texts == ["ACME\n$HH:mm$"]

    panel.text_field.cleared.emit()
    assert texts == ["ACME\n$HH:mm$", ""]
    assert panel.text_field.template() == ""


def test_turning_on_a_watermark_without_text_asks_for_it(qapp, monkeypatch, watermark_settings):
    import ui.watermark_input_dialog as dialog_module
    from ui.toolbar import Toolbar

    watermark_settings.set_screenshot_watermark_text("")
    asked = []
    monkeypatch.setattr(dialog_module, "edit_watermark_text",
                        lambda anchor, manager: asked.append(anchor) or "HELLO")
    toolbar = Toolbar()
    texts = []
    toolbar.watermark_text_changed.connect(texts.append)
    # 截图窗口收到新文字会存下来（WatermarkTool.change_text），这里没有窗口，直接存
    toolbar.watermark_text_changed.connect(watermark_settings.set_screenshot_watermark_text)
    toolbar.watermark_btn.click()
    assert asked == [toolbar.watermark_panel.text_field]
    assert texts == ["HELLO"]

    # 已经有文字就不再追着问
    toolbar.watermark_btn.click()
    toolbar.watermark_btn.click()
    assert len(asked) == 1


def test_input_dialog_inserts_time_at_the_cursor_and_previews(qapp):
    from ui.watermark_input_dialog import TIME_FORMATS, WatermarkInputDialog, time_token

    dialog = WatermarkInputDialog("ACME  ", "yyyy-MM-dd HH:mm:ss")
    cursor = dialog.edit.textCursor()
    cursor.setPosition(4)
    dialog.edit.setTextCursor(cursor)
    dialog.insert_time()
    assert dialog.edit.toPlainText() == "ACME$yyyy-MM-dd HH:mm:ss$  "
    assert dialog.time_button.text() == "ISO 24h"

    # 从菜单选一种格式：直接插进去，按钮也换成它，下次点按钮插的就是这种
    dialog.edit.moveCursor(QTextCursor.MoveOperation.End)
    dialog._fill_time_menu()
    compact = next(a for a in dialog.time_menu.actions() if a.text().startswith("Compact"))
    compact.trigger()
    assert dialog.edit.toPlainText().endswith(time_token("yyyyMMddHHmmss"))
    assert dialog.time_button.text() == "Compact"
    assert dialog.time_format() == "yyyyMMddHHmmss"
    assert len(dialog.time_menu.actions()) == len(TIME_FORMATS)

    assert dialog.template().startswith("ACME$")   # 首尾空白去掉
    assert "$" not in dialog.preview.text().split("ACME", 1)[1]
    assert dialog.preview.isVisibleTo(dialog)
    dialog.edit.setPlainText("")
    assert not dialog.preview.isVisibleTo(dialog)
    dialog.deleteLater()


def test_edit_watermark_text_returns_the_text_and_remembers_the_time_format(qapp, monkeypatch, watermark_settings):
    import ui.watermark_input_dialog as dialog_module

    def fake_exec(dialog, owner=None):
        dialog.edit.setPlainText("A ")
        dialog.edit.moveCursor(QTextCursor.MoveOperation.End)
        dialog.insert_time("HH:mm:ss")
        return accepted[0]

    accepted = [1]
    monkeypatch.setattr(dialog_module, "exec_dialog", fake_exec)
    anchor = QWidget()
    assert dialog_module.edit_watermark_text(anchor, watermark_settings) == "A $HH:mm:ss$"
    assert watermark_settings.get_screenshot_watermark_time_format() == "HH:mm:ss"

    accepted[0] = 0
    assert dialog_module.edit_watermark_text(anchor, watermark_settings) is None


# ---------------------------------------------------------------------------
# 截图窗口
# ---------------------------------------------------------------------------

@pytest.fixture
def window(qapp, tmp_settings, monkeypatch):
    from pin.pin_manager import PinManager
    from settings.tool_settings import ToolSettingsManager
    from ui.screenshot_window import ScreenshotWindow

    monkeypatch.setattr(PinManager, "suppress_topmost", lambda self: None)
    monkeypatch.setattr(PinManager, "restore_topmost", lambda self: None)

    def make(auto=False):
        config = ToolSettingsManager(tmp_settings)
        config.set_screenshot_watermark_auto(auto)
        shot = ScreenshotWindow(config, prefetched_image=_blank(400, 300),
                                prefetched_rect=QRectF(0, 0, 400, 300))
        made.append(shot)
        return shot

    made = []
    yield make
    for shot in made:
        shot.cleanup_and_close()
        shot.deleteLater()


def test_toolbar_button_adds_and_removes_the_watermark_in_a_screenshot(window):
    shot = window()
    shot.scene.preset_selection(QRectF(20, 20, 200, 150))
    assert WatermarkItem.find(shot.scene) is None

    shot.toolbar.watermark_btn.click()
    item = WatermarkItem.find(shot.scene)
    assert item is not None and item.rect() == QRectF(20, 20, 200, 150)

    shot.toolbar.watermark_panel.layout_combo.setCurrentIndex(
        shot.toolbar.watermark_panel.layout_combo.findData("top"))
    assert item.style().layout == "top"
    shot.toolbar.watermark_text_changed.emit("NEW $yyyy$")
    assert item.template() == "NEW $yyyy$"

    shot.toolbar.watermark_btn.click()
    assert WatermarkItem.find(shot.scene) is None
    shot.scene.undo_stack.undo()
    assert WatermarkItem.find(shot.scene) is item


def test_auto_watermark_is_added_once_per_screenshot(window):
    shot = window(auto=True)
    shot.scene.preset_selection(QRectF(20, 20, 200, 150))
    assert WatermarkItem.find(shot.scene) is not None
    assert not shot.scene.undo_stack.canUndo()

    # 切到水印工具再点一下就去掉；重新框选不会再自己冒出来
    shot.toolbar.watermark_btn.click()
    shot.toolbar.watermark_btn.click()
    assert WatermarkItem.find(shot.scene) is None
    shot.scene.preset_selection(QRectF(30, 30, 100, 100))
    assert WatermarkItem.find(shot.scene) is None


def test_auto_watermark_needs_some_text(window, watermark_settings):
    watermark_settings.set_screenshot_watermark_text("")
    shot = window(auto=True)
    shot.scene.preset_selection(QRectF(20, 20, 200, 150))
    assert WatermarkItem.find(shot.scene) is None


def test_no_watermark_unless_asked(window):
    shot = window(auto=False)
    shot.scene.preset_selection(QRectF(20, 20, 200, 150))
    assert WatermarkItem.find(shot.scene) is None


def test_screenshot_shortcuts_yield_to_the_watermark_input_dialog(window):
    """输入框开着时截图快捷键要让出键盘：否则在框里按 R 会切矩形工具、按 Enter 会确认截图"""
    from PySide6.QtCore import QTimer
    from PySide6.QtWidgets import QApplication
    from ui.watermark_input_dialog import WatermarkInputDialog

    shot = window()
    shot.show()
    shot.scene.preset_selection(QRectF(20, 20, 200, 150))
    shot.toolbar.watermark_btn.click()
    assert shot._shortcut_handler.is_active()

    observed = []

    def probe():
        [dialog] = [w for w in QApplication.topLevelWidgets()
                    if isinstance(w, WatermarkInputDialog) and w.isVisible()]
        observed.append((shot._shortcut_handler.is_active(), dialog.edit.toPlainText()))
        dialog.reject()

    QTimer.singleShot(0, probe)
    shot.toolbar.edit_watermark_text()
    assert observed == [(False, "WATERMARK")]
    assert shot._shortcut_handler.is_active()
