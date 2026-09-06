import pytest

from PySide6.QtCore import QPoint, QPointF, QRectF, QSize, Qt
from PySide6.QtGui import QColor, QImage, QPainter, QPainterPath
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QWidget

from canvas.scene import CanvasScene
from core.export import ExportService


def _gradient_image(width=64, height=64):
    image = QImage(width, height, QImage.Format.Format_ARGB32)
    for y in range(height):
        for x in range(width):
            image.setPixelColor(x, y, QColor((x * 3) % 256, (y * 5) % 256, (x + y) % 256))
    return image


def _assert_images_close(actual, expected, tolerance=4):
    assert actual.size() == expected.size()
    for y in range(actual.height()):
        for x in range(actual.width()):
            left = actual.pixelColor(x, y)
            right = expected.pixelColor(x, y)
            assert max(
                abs(left.red() - right.red()),
                abs(left.green() - right.green()),
                abs(left.blue() - right.blue()),
                abs(left.alpha() - right.alpha()),
            ) <= tolerance


def _draw_mosaic(scene, start=QPointF(8, 32), end=QPointF(56, 32), width=16):
    scene.activate_tool("mosaic")
    scene.update_style(width=width)
    controller = scene.tool_controller
    assert controller.on_press(start, Qt.MouseButton.LeftButton) is True
    controller.on_move(end)
    controller.on_release(end)


def test_mosaic_registration_is_explicit(qapp):
    image = _gradient_image()
    default_scene = CanvasScene(image, QRectF(0, 0, 64, 64))
    screenshot_scene = CanvasScene(image, QRectF(0, 0, 64, 64), enable_mosaic=True)

    assert default_scene.tool_controller.get_tool("mosaic") is None
    assert screenshot_scene.tool_controller.get_tool("mosaic") is not None


def test_single_point_mosaic_has_real_round_geometry(qapp):
    from canvas.items import MosaicItem

    path = QPainterPath(QPointF(20, 20))
    reduced = _gradient_image(5, 5)
    item = MosaicItem(path, 18, 8, reduced, QRectF(0, 0, 40, 40))

    assert not item.shape().isEmpty()
    assert item.boundingRect().width() == 18
    assert item.boundingRect().height() == 18
    assert item.shape().contains(QPointF(20, 20))


def test_mosaic_export_undo_redo_and_cropped_patch(qapp):
    from canvas.items import MosaicItem

    source = _gradient_image()
    scene = CanvasScene(source, QRectF(0, 0, 64, 64), enable_mosaic=True)
    scene.selection_model.initialize_confirmed_rect(QRectF(0, 0, 64, 64))

    _draw_mosaic(scene)

    items = [item for item in scene.items() if isinstance(item, MosaicItem)]
    assert len(items) == 1
    # 小图按 block 缩小，体积是全分辨率的 1/block_size**2
    assert items[0].reduced_image().sizeInBytes() * 32 < source.sizeInBytes()
    assert scene.undo_stack.count() == 1

    rendered = ExportService(scene).export(QRectF(0, 0, 64, 64))
    assert rendered.pixelColor(4, 4) == source.pixelColor(4, 4)
    assert rendered.pixelColor(16, 32) == rendered.pixelColor(17, 32)
    assert rendered.pixelColor(16, 32) != source.pixelColor(16, 32)

    scene.undo_stack.undo()
    restored = ExportService(scene).export(QRectF(0, 0, 64, 64))
    assert restored.pixelColor(16, 32) == source.pixelColor(16, 32)
    scene.undo_stack.redo()
    rerendered = ExportService(scene).export(QRectF(0, 0, 64, 64))
    assert rerendered.pixelColor(16, 32) == rendered.pixelColor(16, 32)


def test_mosaic_aligns_with_negative_scene_origin(qapp):
    from canvas.items import MosaicItem

    source = _gradient_image()
    scene_rect = QRectF(-32, -16, 64, 64)
    scene = CanvasScene(source, scene_rect, enable_mosaic=True)
    scene.selection_model.initialize_confirmed_rect(scene_rect)
    _draw_mosaic(scene, QPointF(-24, 16), QPointF(24, 16))

    item = next(item for item in scene.items() if isinstance(item, MosaicItem))
    assert item.background_rect().left() < 0
    rendered = ExportService(scene).export(scene_rect)
    assert rendered.pixelColor(16, 32) == rendered.pixelColor(17, 32)
    assert rendered.pixelColor(4, 4) == source.pixelColor(4, 4)


def test_pixelated_blocks_stay_aligned_for_non_divisible_image_size(qapp):
    """小图是在 paint 时才铺开的，所以这里检查最终画面而不是中间产物。"""
    source = _gradient_image(65, 65)
    scene = CanvasScene(source, QRectF(0, 0, 65, 65), enable_mosaic=True)
    scene.selection_model.initialize_confirmed_rect(QRectF(0, 0, 65, 65))
    _draw_mosaic(scene, QPointF(0, 10), QPointF(64, 10), width=30)
    rendered = ExportService(scene).export(QRectF(0, 0, 65, 65))

    first_block = rendered.pixelColor(0, 10)
    second_block = rendered.pixelColor(8, 10)
    assert all(rendered.pixelColor(x, 10) == first_block for x in range(0, 8))
    assert all(rendered.pixelColor(x, 10) == second_block for x in range(8, 16))
    assert first_block != second_block

    # 右边缘不足一个 block 的余数只按实际存在的那一列取值，不能把邻块混进来。
    edge_source = QImage(65, 8, QImage.Format.Format_ARGB32)
    edge_source.fill(QColor("black"))
    for y in range(edge_source.height()):
        edge_source.setPixelColor(64, y, QColor("white"))
    edge_scene = CanvasScene(edge_source, QRectF(0, 0, 65, 8), enable_mosaic=True)
    edge_scene.selection_model.initialize_confirmed_rect(QRectF(0, 0, 65, 8))
    _draw_mosaic(edge_scene, QPointF(0, 4), QPointF(64, 4), width=30)
    edge_rendered = ExportService(edge_scene).export(QRectF(0, 0, 65, 8))

    assert edge_rendered.pixelColor(63, 4) == QColor("black")
    assert edge_rendered.pixelColor(64, 4) == QColor("white")


def test_mosaic_press_failure_is_atomic(monkeypatch, qapp):
    from canvas.items import MosaicItem

    scene = CanvasScene(_gradient_image(), QRectF(0, 0, 64, 64), enable_mosaic=True)
    scene.activate_tool("mosaic")
    tool = scene.tool_controller.current_tool
    monkeypatch.setattr(scene.background, "reduced_image", lambda _size: (_ for _ in ()).throw(RuntimeError("boom")))

    assert scene.tool_controller.on_press(QPointF(10, 10), Qt.MouseButton.LeftButton) is False
    assert tool.current_item is None
    assert not any(isinstance(item, MosaicItem) for item in scene.items())
    assert scene.undo_stack.count() == 0


def test_mosaic_release_failure_removes_live_item(monkeypatch, qapp):
    from canvas.items import MosaicItem

    scene = CanvasScene(_gradient_image(), QRectF(0, 0, 64, 64), enable_mosaic=True)
    scene.activate_tool("mosaic")
    tool = scene.tool_controller.current_tool
    assert scene.tool_controller.on_press(QPointF(10, 10), Qt.MouseButton.LeftButton) is True
    monkeypatch.setattr(tool.current_item, "set_path", lambda *_args: (_ for _ in ()).throw(RuntimeError("boom")))

    scene.tool_controller.on_release(QPointF(20, 20))

    assert tool.current_item is None
    assert not any(isinstance(item, MosaicItem) for item in scene.items())
    assert scene.undo_stack.count() == 0


def test_mosaic_commit_does_not_remove_live_item(monkeypatch, qapp):
    from canvas.items import MosaicItem

    scene = CanvasScene(_gradient_image(), QRectF(0, 0, 64, 64), enable_mosaic=True)
    scene.activate_tool("mosaic")
    assert scene.tool_controller.on_press(QPointF(10, 10), Qt.MouseButton.LeftButton) is True
    monkeypatch.setattr(scene, "removeItem", lambda *_args: (_ for _ in ()).throw(RuntimeError("unexpected remove")))

    scene.tool_controller.on_release(QPointF(20, 20))

    assert scene.undo_stack.count() == 1
    assert any(isinstance(item, MosaicItem) for item in scene.items())


def test_mosaic_push_failure_is_atomic(monkeypatch, qapp):
    from canvas.items import MosaicItem

    scene = CanvasScene(_gradient_image(), QRectF(0, 0, 64, 64), enable_mosaic=True)
    scene.activate_tool("mosaic")
    tool = scene.tool_controller.current_tool
    assert scene.tool_controller.on_press(QPointF(10, 10), Qt.MouseButton.LeftButton) is True
    monkeypatch.setattr(scene.undo_stack, "push_command", lambda *_args: (_ for _ in ()).throw(RuntimeError("boom")))

    scene.tool_controller.on_release(QPointF(20, 20))

    assert tool.current_item is None
    assert not any(isinstance(item, MosaicItem) for item in scene.items())
    assert scene.undo_stack.count() == 0


def test_mosaic_push_post_commit_failure_keeps_one_coherent_command(monkeypatch, qapp):
    from canvas.items import MosaicItem

    scene = CanvasScene(_gradient_image(), QRectF(0, 0, 64, 64), enable_mosaic=True)
    scene.activate_tool("mosaic")
    tool = scene.tool_controller.current_tool
    assert scene.tool_controller.on_press(QPointF(10, 10), Qt.MouseButton.LeftButton) is True
    original_push = scene.undo_stack.push_command

    def push_then_raise(command):
        original_push(command)
        raise RuntimeError("after commit")

    monkeypatch.setattr(scene.undo_stack, "push_command", push_then_raise)
    scene.tool_controller.on_release(QPointF(20, 20))

    assert tool.current_item is None
    assert scene.undo_stack.count() == 1
    assert scene.undo_stack.index() == 1
    assert len([item for item in scene.items() if isinstance(item, MosaicItem)]) == 1


def test_mosaic_push_exception_with_different_command_does_not_claim_ownership(monkeypatch, qapp):
    from canvas.items import MosaicItem, StrokeItem
    from canvas.undo import AddItemCommand
    from PySide6.QtGui import QPen

    scene = CanvasScene(_gradient_image(), QRectF(0, 0, 64, 64), enable_mosaic=True)
    scene.activate_tool("mosaic")
    assert scene.tool_controller.on_press(QPointF(10, 10), Qt.MouseButton.LeftButton) is True
    original_push = scene.undo_stack.push_command
    path = QPainterPath(QPointF(1, 1))
    path.lineTo(QPointF(2, 2))
    other = StrokeItem(path, QPen(QColor("red"), 1))

    def push_other_then_raise(_command):
        original_push(AddItemCommand(scene, other))
        raise RuntimeError("different command committed")

    monkeypatch.setattr(scene.undo_stack, "push_command", push_other_then_raise)
    scene.tool_controller.on_release(QPointF(20, 20))

    assert not any(isinstance(item, MosaicItem) for item in scene.items())
    assert scene.undo_stack.command(0).item is other


def test_mosaic_null_reduced_image_cleans_view_and_next_stroke_works(monkeypatch, qapp):
    from canvas.items import MosaicItem
    from canvas.view import CanvasView

    scene = CanvasScene(_gradient_image(), QRectF(0, 0, 64, 64), enable_mosaic=True)
    scene.selection_model.initialize_confirmed_rect(QRectF(0, 0, 64, 64))
    parent = QWidget()
    parent.resize(64, 64)
    view = CanvasView(scene, parent)
    view.setGeometry(0, 0, 64, 64)
    parent.show()
    qapp.processEvents()
    scene.activate_tool("mosaic")
    tool = scene.tool_controller.current_tool
    original_reduced = scene.background.reduced_image
    monkeypatch.setattr(scene.background, "reduced_image", lambda _size: QImage())

    QTest.mousePress(view.viewport(), Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, QPoint(20, 20))
    QTest.mouseRelease(view.viewport(), Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, QPoint(20, 20))

    assert view.is_drawing is False
    assert tool.current_item is None
    assert scene.undo_stack.count() == 0

    monkeypatch.setattr(scene.background, "reduced_image", original_reduced)
    QTest.mousePress(view.viewport(), Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, QPoint(20, 20))
    QTest.mouseMove(view.viewport(), QPoint(36, 20))
    QTest.mouseRelease(view.viewport(), Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, QPoint(36, 20))

    assert view.is_drawing is False
    assert scene.undo_stack.count() == 1
    assert any(isinstance(item, MosaicItem) for item in scene.items())
    parent.close()


def test_pin_registers_mosaic_and_can_draw_with_it(qapp):
    """钉图涂的是钉住的那张图，和截图场景没有区别，所以马赛克在这里同样可用。"""
    from canvas.items import MosaicItem
    from pin.pin_canvas import PinCanvas

    parent = QWidget()
    parent._is_editing = False
    parent.toolbar = None
    canvas = PinCanvas(parent, QSize(64, 64), _gradient_image())

    assert canvas.tool_controller.get_tool("mosaic") is not None
    assert canvas.activate_tool("mosaic") is True
    assert canvas.is_editing is True

    _draw_mosaic(canvas.scene)
    drawn = [item for item in canvas.scene.items() if isinstance(item, MosaicItem)]
    assert len(drawn) == 1
    # 钉图场景原点就是 (0,0)，背景锚点因此天然对齐，不需要额外平移
    assert drawn[0].background_rect() == canvas.scene.scene_rect


def test_pin_still_refuses_tools_its_scene_never_registered(qapp):
    """"钉图支持什么"只有一条判据：这个场景注册了没有。"""
    from pin.pin_canvas import PinCanvas

    parent = QWidget()
    parent._is_editing = False
    parent.toolbar = None
    canvas = PinCanvas(parent, QSize(64, 64), _gradient_image())
    before = canvas.tool_controller.current_tool

    assert canvas.activate_tool("gif") is False
    assert canvas.tool_controller.current_tool is before
    assert canvas.is_editing is False


def test_eraser_removes_and_restores_mosaic(qapp):
    from canvas.items import MosaicItem

    scene = CanvasScene(_gradient_image(), QRectF(0, 0, 64, 64), enable_mosaic=True)
    _draw_mosaic(scene)
    assert any(isinstance(item, MosaicItem) for item in scene.items())
    scene.activate_tool("eraser")
    scene.update_style(width=20)

    controller = scene.tool_controller
    controller.on_press(QPointF(32, 32), Qt.MouseButton.LeftButton)
    controller.on_release(QPointF(32, 32))

    assert not any(isinstance(item, MosaicItem) for item in scene.items())
    scene.undo_stack.undo()
    assert any(isinstance(item, MosaicItem) for item in scene.items())


def test_eraser_click_removes_all_overlapping_items(qapp):
    from canvas.items import MosaicItem, StrokeItem
    from canvas.undo import AddItemCommand
    from PySide6.QtGui import QPen

    scene = CanvasScene(_gradient_image(), QRectF(0, 0, 64, 64), enable_mosaic=True)
    _draw_mosaic(scene, QPointF(20, 32), QPointF(44, 32), width=16)
    path = QPainterPath(QPointF(20, 32))
    path.lineTo(QPointF(44, 32))
    stroke = StrokeItem(path, QPen(QColor("red"), 8))
    scene.undo_stack.push(AddItemCommand(scene, stroke))
    scene.activate_tool("eraser")
    scene.update_style(width=20)

    controller = scene.tool_controller
    controller.on_press(QPointF(32, 32), Qt.MouseButton.LeftButton)
    controller.on_release(QPointF(32, 32))

    assert not any(isinstance(item, (MosaicItem, StrokeItem)) for item in scene.items())
    scene.undo_stack.undo()
    assert any(isinstance(item, MosaicItem) for item in scene.items())
    assert stroke.scene() is scene


def test_eraser_drag_removes_all_overlapping_items(qapp):
    from canvas.items import StrokeItem
    from canvas.undo import AddItemCommand
    from PySide6.QtGui import QPen

    scene = CanvasScene(_gradient_image(), QRectF(0, 0, 64, 64), enable_mosaic=True)
    strokes = []
    for color in ("red", "blue"):
        path = QPainterPath(QPointF(20, 32))
        path.lineTo(QPointF(44, 32))
        item = StrokeItem(path, QPen(QColor(color), 6))
        scene.undo_stack.push(AddItemCommand(scene, item))
        strokes.append(item)
    scene.activate_tool("eraser")
    scene.update_style(width=16)

    controller = scene.tool_controller
    controller.on_press(QPointF(10, 32), Qt.MouseButton.LeftButton)
    controller.on_move(QPointF(54, 32))
    controller.on_release(QPointF(54, 32))

    assert all(item.scene() is None for item in strokes)
    scene.undo_stack.undo()
    assert all(item.scene() is scene for item in strokes)


def test_mosaic_z_order_keeps_normal_annotations_above(qapp):
    from canvas.items import MosaicItem, StrokeItem
    from canvas.undo import AddItemCommand
    from PySide6.QtGui import QPen

    scene = CanvasScene(_gradient_image(), QRectF(0, 0, 64, 64), enable_mosaic=True)
    _draw_mosaic(scene, QPointF(16, 32), QPointF(48, 32), width=18)
    mosaic = next(item for item in scene.items() if isinstance(item, MosaicItem))
    path = QPainterPath(QPointF(16, 32))
    path.lineTo(QPointF(48, 32))
    stroke = StrokeItem(path, QPen(QColor("red"), 4))
    scene.undo_stack.push(AddItemCommand(scene, stroke))

    rendered = ExportService(scene).export(QRectF(0, 0, 64, 64))

    assert mosaic.zValue() == 5
    assert stroke.zValue() > mosaic.zValue()
    assert rendered.pixelColor(32, 32).red() > 200


def test_reduced_cache_invalidates_on_background_change_and_release(qapp):
    scene = CanvasScene(_gradient_image(), QRectF(0, 0, 64, 64), enable_mosaic=True)
    original = scene.background.reduced_image(8)
    replacement = QImage(64, 64, QImage.Format.Format_ARGB32)
    replacement.fill(QColor("red"))

    scene.background.update_image(replacement)
    updated = scene.background.reduced_image(8)
    scene.background.release_image_cache()
    rebuilt = scene.background.reduced_image(8)

    assert updated != original
    assert updated.pixelColor(2, 2) == QColor("red")
    assert rebuilt == updated


def test_reduced_cache_keeps_only_latest_block_size(monkeypatch, qapp):
    """粒度缓存只留最近一次算出来的那张，换粒度就把旧的顶掉，不按 block_size 攒字典。"""
    from PIL import Image

    import canvas.items.background_item as background_item_module

    scene = CanvasScene(_gradient_image(), QRectF(0, 0, 64, 64))
    background = scene.background

    # frombytes 只在真正重新收缩（缓存未命中）时才会被调用，是比 hook
    # PIL.Image.reduce（内部可能自我递归）更安全的探针。
    calls = []
    original_frombytes = Image.frombytes

    def counting_frombytes(*args, **kwargs):
        calls.append(True)
        return original_frombytes(*args, **kwargs)

    monkeypatch.setattr(background_item_module.Image, "frombytes", counting_frombytes)

    background.reduced_image(8)
    background.reduced_image(8)  # 同一粒度命中缓存，不重新算
    assert len(calls) == 1

    background.reduced_image(16)  # 换粒度，必须重新算
    assert len(calls) == 2

    background.reduced_image(8)  # 8 那份已经被 16 顶掉了，不是"两份都留着"
    assert len(calls) == 3


def test_mosaic_item_set_block_size_swaps_reduced_image():
    from canvas.items import MosaicItem

    path = QPainterPath(QPointF(10, 10))
    reduced_8 = _gradient_image(5, 5)
    item = MosaicItem(path, 18, 8, reduced_8, QRectF(0, 0, 40, 40))

    reduced_16 = _gradient_image(3, 3)
    item.set_block_size(16, reduced_16)

    assert item.block_size() == 16
    assert item.reduced_image().size() == reduced_16.size()


def test_mosaic_block_size_change_is_undoable():
    from canvas.items import MosaicItem
    from canvas.undo import EditItemCommand

    path = QPainterPath(QPointF(10, 10))
    reduced_8 = _gradient_image(5, 5)
    item = MosaicItem(path, 18, 8, reduced_8, QRectF(0, 0, 40, 40))

    reduced_16 = _gradient_image(3, 3)
    old_state = {"block_size": 8, "reduced_image": item.reduced_image()}
    item.set_block_size(16, reduced_16)
    new_state = {"block_size": 16, "reduced_image": item.reduced_image()}
    command = EditItemCommand(item, old_state, new_state, "Change Mosaic Size")

    command.undo()
    assert item.block_size() == 8
    assert item.reduced_image().size() == reduced_8.size()

    command.redo()
    assert item.block_size() == 16
    assert item.reduced_image().size() == reduced_16.size()


def test_screenshot_mosaic_clones_into_pin(qapp):
    from canvas.items import MosaicItem
    from pin.pin_canvas import PinCanvas

    source_scene = CanvasScene(_gradient_image(), QRectF(0, 0, 64, 64), enable_mosaic=True)
    _draw_mosaic(source_scene, QPointF(16, 32), QPointF(48, 32), width=12)
    source_item = next(item for item in source_scene.items() if isinstance(item, MosaicItem))

    parent = QWidget()
    parent._is_editing = False
    parent.toolbar = None
    selection = QRectF(8, 0, 48, 64)
    pin = PinCanvas(parent, QSize(48, 64), source_scene.background.image().copy(8, 0, 48, 64))
    pin.initialize_from_items([source_item], QPoint(8, 0))

    cloned = next(item for item in pin.scene.items() if isinstance(item, MosaicItem))
    assert cloned.block_size() == source_item.block_size()
    assert cloned.brush_width() == source_item.brush_width()
    assert cloned.reduced_image() == source_item.reduced_image()
    assert cloned.pos() == QPointF(-8, 0)
    assert cloned.sceneBoundingRect().center().x() == source_item.sceneBoundingRect().center().x() - 8

    expected = ExportService(source_scene).export(selection)
    rendered = QImage(48, 64, QImage.Format.Format_ARGB32)
    rendered.fill(Qt.GlobalColor.transparent)
    painter = QPainter(rendered)
    pin.render_to_painter(painter, QRectF(0, 0, 48, 64))
    painter.end()
    rendered = rendered.convertToFormat(expected.format())
    _assert_images_close(rendered, expected)

    from pin.pin_image_transform import PinImageTransform

    for mutate in (
        lambda transform: transform.rotate_cw(),
        lambda transform: transform.rotate_ccw(),
        lambda transform: transform.flip_horizontal(),
        lambda transform: transform.flip_vertical(),
    ):
        transform = PinImageTransform()
        mutate(transform)
        _assert_images_close(
            transform.transform_image(rendered),
            transform.transform_image(expected),
        )


def test_cloned_mosaic_background_anchor_follows_the_selection_offset(qapp):
    """马赛克的背景锚点是场景坐标，换场景时必须和 pos 一起搬。

    只平移 pos 的话，钉图里的马赛克画的是偏移了一整个选区的那一块背景——
    形状和位置都对，露出来的内容却是别处的。
    """
    from canvas.items import MosaicItem
    from pin.pin_canvas import PinCanvas

    source_scene = CanvasScene(_gradient_image(), QRectF(0, 0, 64, 64), enable_mosaic=True)
    _draw_mosaic(source_scene, QPointF(16, 32), QPointF(48, 32), width=12)
    source_item = next(item for item in source_scene.items() if isinstance(item, MosaicItem))

    parent = QWidget()
    parent._is_editing = False
    parent.toolbar = None
    offset = QPoint(8, 0)
    pin = PinCanvas(parent, QSize(48, 64), source_scene.background.image().copy(8, 0, 48, 64))
    pin.initialize_from_items([source_item], offset)

    cloned = next(item for item in pin.scene.items() if isinstance(item, MosaicItem))
    assert cloned.background_rect() == source_item.background_rect().translated(
        -offset.x(), -offset.y()
    )


# ---------------------------------------------------------------------------
# 框选模式：整条路径此前没有测试走过（隔离后的默认设置是自由涂抹）
# ---------------------------------------------------------------------------

@pytest.fixture
def rect_mode():
    """把马赛克切到框选模式，用完还原。"""
    from settings import get_tool_settings_manager
    from tools.mosaic import MosaicTool

    manager = get_tool_settings_manager()
    before = manager.get_setting("mosaic", "draw_mode", MosaicTool.MODE_FREEHAND)
    manager.update_settings("mosaic", draw_mode=MosaicTool.MODE_RECT)
    yield
    manager.update_settings("mosaic", draw_mode=before)


def _rect_scene():
    scene = CanvasScene(_gradient_image(), QRectF(0, 0, 64, 64), enable_mosaic=True)
    scene.selection_model.initialize_confirmed_rect(QRectF(0, 0, 64, 64))
    return scene


def test_rect_mosaic_fills_the_dragged_rectangle(qapp, rect_mode):
    from canvas.items import MosaicItem

    scene = _rect_scene()
    _draw_mosaic(scene, QPointF(10, 10), QPointF(50, 40))

    item = next(i for i in scene.items() if isinstance(i, MosaicItem))
    assert item.fill_mode() is True
    # 框选的形状就是拖出来的矩形本身，不再经画笔描边
    assert item.rect() == QRectF(10, 10, 40, 30)
    assert scene.undo_stack.count() == 1


def test_rect_mosaic_auto_selects_itself_but_freehand_does_not(qapp, rect_mode):
    """框选画完直接选中方便调尺寸；自由涂抹沿用旧行为。"""
    from settings import get_tool_settings_manager
    from tools.mosaic import MosaicTool

    scene = _rect_scene()
    selected = []
    scene.item_auto_select_requested.connect(selected.append)

    _draw_mosaic(scene, QPointF(10, 10), QPointF(50, 40))
    assert len(selected) == 1

    get_tool_settings_manager().update_settings(
        "mosaic", draw_mode=MosaicTool.MODE_FREEHAND
    )
    _draw_mosaic(scene, QPointF(10, 50), QPointF(50, 55))
    assert len(selected) == 1


def test_a_tiny_rect_drag_is_treated_as_a_misclick(qapp, rect_mode):
    """比 MIN_SIZE 还小的拖拽当误触丢弃，不留图元也不进撤销栈。"""
    from canvas.items import MosaicItem

    scene = _rect_scene()
    _draw_mosaic(scene, QPointF(10, 10), QPointF(14, 14))

    assert not any(isinstance(i, MosaicItem) for i in scene.items())
    assert scene.undo_stack.count() == 0


def test_dragging_a_rect_mosaic_handle_resizes_it(qapp, rect_mode):
    """框选马赛克复用 handle_editor 认 rect()/setRect() 的通用缩放。"""
    from canvas.items import MosaicItem

    scene = _rect_scene()
    _draw_mosaic(scene, QPointF(10, 10), QPointF(50, 40))
    item = next(i for i in scene.items() if isinstance(i, MosaicItem))

    item.setRect(QRectF(0, 0, 20, 20))
    assert item.rect() == QRectF(0, 0, 20, 20)

    # 自由涂抹没有"矩形"可言，setRect 对它无意义
    freehand = MosaicItem(QPainterPath(QPointF(5, 5)), 8, 8, _gradient_image(4, 4),
                          QRectF(0, 0, 64, 64), fill_mode=False)
    before = freehand.path()
    freehand.setRect(QRectF(0, 0, 30, 30))
    assert freehand.path() == before


# ---------------------------------------------------------------------------
# 共用策略：截图窗口和钉图窗口都只调这两个入口，所以直接对它们测
# ---------------------------------------------------------------------------

class _FakeView:
    """只提供 apply_* 用到的那一条链路：view.smart_edit_controller.selected_item"""

    def __init__(self, item=None):
        self.smart_edit_controller = type("C", (), {"selected_item": item})()


def _mosaic_scene_with_selection():
    from canvas.items import MosaicItem

    scene = CanvasScene(_gradient_image(), QRectF(0, 0, 64, 64), enable_mosaic=True)
    scene.selection_model.initialize_confirmed_rect(QRectF(0, 0, 64, 64))
    _draw_mosaic(scene)
    item = next(i for i in scene.items() if isinstance(i, MosaicItem))
    return scene, item, _FakeView(item)


def test_style_change_needs_a_selected_mosaic(qapp):
    from tools.mosaic import MosaicTool

    scene, item, view = _mosaic_scene_with_selection()
    stack = scene.undo_stack
    before = stack.count()

    assert MosaicTool.apply_style_change("blur", _FakeView(None), stack) is False
    assert MosaicTool.apply_style_change("blur", _FakeView(object()), stack) is False
    assert MosaicTool.apply_style_change("blur", None, stack) is False
    assert stack.count() == before


def test_style_change_is_applied_once_and_is_undoable(qapp):
    from tools.mosaic import MosaicTool

    scene, item, view = _mosaic_scene_with_selection()
    stack = scene.undo_stack
    before = stack.count()

    assert item.smooth() is False
    assert MosaicTool.apply_style_change("blur", view, stack) is True
    assert item.smooth() is True
    assert stack.count() == before + 1

    # 设成同一个值不该再产生命令
    assert MosaicTool.apply_style_change("blur", view, stack) is False
    assert stack.count() == before + 1

    stack.undo()
    assert item.smooth() is False


def test_block_size_change_swaps_the_reduced_image_and_is_undoable(qapp):
    from tools.mosaic import MosaicTool

    scene, item, view = _mosaic_scene_with_selection()
    stack = scene.undo_stack
    old_size, old_image = item.block_size(), item.reduced_image()

    assert MosaicTool.apply_block_size_change(old_size + 8, view, stack) is True
    assert item.block_size() == old_size + 8
    # 粒度变了，配套的小图必须一起换成同一粒度那张
    assert item.reduced_image().size() != old_image.size()

    stack.undo()
    assert item.block_size() == old_size
    assert item.reduced_image().size() == old_image.size()


def test_block_size_is_clamped_and_garbage_falls_back(qapp):
    from tools.mosaic import MosaicTool

    assert MosaicTool.clamp_block_size(0) == MosaicTool.MIN_BLOCK_SIZE
    assert MosaicTool.clamp_block_size(9999) == MosaicTool.MAX_BLOCK_SIZE
    assert MosaicTool.clamp_block_size("八") == MosaicTool.DEFAULT_BLOCK_SIZE
    assert MosaicTool.clamp_block_size(None) == MosaicTool.DEFAULT_BLOCK_SIZE

    scene, item, view = _mosaic_scene_with_selection()
    MosaicTool.apply_block_size_change(9999, view, scene.undo_stack)
    assert item.block_size() == MosaicTool.MAX_BLOCK_SIZE


def test_applying_without_an_undo_stack_still_changes_the_item(qapp):
    """钉图早期阶段可能还没有撤销栈；没有栈不该让改动本身失败。"""
    from tools.mosaic import MosaicTool

    scene, item, view = _mosaic_scene_with_selection()
    assert MosaicTool.apply_style_change("blur", view, None) is True
    assert item.smooth() is True


def test_settings_fall_back_to_defaults_without_a_manager(qapp):
    """没有 settings_manager 时读到的必须是包内默认值，而不是抛异常。"""
    from tools.mosaic import MosaicTool

    ctx = type("Ctx", (), {"settings_manager": None})()
    assert MosaicTool.get_draw_mode(ctx) == MosaicTool.MODE_FREEHAND
    assert MosaicTool.get_style(ctx) == MosaicTool.STYLE_PIXELATE
    assert MosaicTool.get_block_size(ctx) == MosaicTool.DEFAULT_BLOCK_SIZE
