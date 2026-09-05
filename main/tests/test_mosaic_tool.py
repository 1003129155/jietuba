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
    patch = _gradient_image(40, 40)
    item = MosaicItem(path, 18, 8, patch, QPointF(0, 0))

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
    assert items[0].patch_image().sizeInBytes() < source.sizeInBytes()
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
    assert item.patch_origin().x() < 0
    rendered = ExportService(scene).export(scene_rect)
    assert rendered.pixelColor(16, 32) == rendered.pixelColor(17, 32)
    assert rendered.pixelColor(4, 4) == source.pixelColor(4, 4)


def test_pixelated_blocks_stay_aligned_for_non_divisible_image_size(qapp):
    source = _gradient_image(65, 65)
    scene = CanvasScene(source, QRectF(0, 0, 65, 65), enable_mosaic=True)

    pixelated = scene.background.pixelated_image(8)

    first_block = pixelated.pixelColor(0, 10)
    second_block = pixelated.pixelColor(8, 10)
    assert all(pixelated.pixelColor(x, 10) == first_block for x in range(0, 8))
    assert all(pixelated.pixelColor(x, 10) == second_block for x in range(8, 16))
    assert first_block != second_block

    edge_source = QImage(65, 8, QImage.Format.Format_ARGB32)
    edge_source.fill(QColor("black"))
    for y in range(edge_source.height()):
        edge_source.setPixelColor(64, y, QColor("white"))
    edge_scene = CanvasScene(edge_source, QRectF(0, 0, 65, 8), enable_mosaic=True)
    edge_pixelated = edge_scene.background.pixelated_image(8)

    assert edge_pixelated.pixelColor(63, 4) == QColor("black")
    assert edge_pixelated.pixelColor(64, 4) == QColor("white")


def test_mosaic_press_failure_is_atomic(monkeypatch, qapp):
    from canvas.items import MosaicItem

    scene = CanvasScene(_gradient_image(), QRectF(0, 0, 64, 64), enable_mosaic=True)
    scene.activate_tool("mosaic")
    tool = scene.tool_controller.current_tool
    monkeypatch.setattr(scene.background, "pixelated_image", lambda _size: (_ for _ in ()).throw(RuntimeError("boom")))

    assert scene.tool_controller.on_press(QPointF(10, 10), Qt.MouseButton.LeftButton) is False
    assert tool.current_item is None
    assert tool.full_image is None
    assert not any(isinstance(item, MosaicItem) for item in scene.items())
    assert scene.undo_stack.count() == 0


def test_mosaic_release_failure_removes_live_item(monkeypatch, qapp):
    from canvas.items import MosaicItem

    scene = CanvasScene(_gradient_image(), QRectF(0, 0, 64, 64), enable_mosaic=True)
    scene.activate_tool("mosaic")
    tool = scene.tool_controller.current_tool
    assert scene.tool_controller.on_press(QPointF(10, 10), Qt.MouseButton.LeftButton) is True
    monkeypatch.setattr(tool, "_crop_patch", lambda *_args: (_ for _ in ()).throw(RuntimeError("boom")))

    scene.tool_controller.on_release(QPointF(20, 20))

    assert tool.current_item is None
    assert tool.full_image is None
    assert not any(isinstance(item, MosaicItem) for item in scene.items())
    assert scene.undo_stack.count() == 0


def test_mosaic_set_patch_failure_is_atomic(monkeypatch, qapp):
    from canvas.items import MosaicItem

    scene = CanvasScene(_gradient_image(), QRectF(0, 0, 64, 64), enable_mosaic=True)
    scene.activate_tool("mosaic")
    tool = scene.tool_controller.current_tool
    assert scene.tool_controller.on_press(QPointF(10, 10), Qt.MouseButton.LeftButton) is True
    monkeypatch.setattr(tool.current_item, "set_patch", lambda *_args: (_ for _ in ()).throw(RuntimeError("boom")))

    scene.tool_controller.on_release(QPointF(20, 20))

    assert tool.current_item is None
    assert tool.full_image is None
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
    assert tool.full_image is None
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
    assert tool.full_image is None
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


def test_mosaic_null_patch_cleans_view_and_next_stroke_works(monkeypatch, qapp):
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
    original_crop = tool._crop_patch
    monkeypatch.setattr(tool, "_crop_patch", lambda *_args: (QImage(), QPointF()))

    QTest.mousePress(view.viewport(), Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, QPoint(20, 20))
    QTest.mouseRelease(view.viewport(), Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, QPoint(20, 20))

    assert view.is_drawing is False
    assert tool.current_item is None
    assert tool.full_image is None
    assert scene.undo_stack.count() == 0

    monkeypatch.setattr(tool, "_crop_patch", original_crop)
    QTest.mousePress(view.viewport(), Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, QPoint(20, 20))
    QTest.mouseMove(view.viewport(), QPoint(36, 20))
    QTest.mouseRelease(view.viewport(), Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, QPoint(36, 20))

    assert view.is_drawing is False
    assert scene.undo_stack.count() == 1
    assert any(isinstance(item, MosaicItem) for item in scene.items())
    parent.close()


def test_pin_does_not_register_or_activate_mosaic(qapp):
    from pin.pin_canvas import PinCanvas

    parent = QWidget()
    parent._is_editing = False
    parent.toolbar = None
    parent._ocr_mgr = type("OcrManager", (), {"set_drawing_mode": lambda self, value: setattr(self, "mode", value)})()
    parent._ocr_mgr.mode = False
    canvas = PinCanvas(parent, QSize(64, 64), _gradient_image())
    before = canvas.tool_controller.current_tool

    assert canvas.activate_tool("mosaic") is False
    canvas._on_tool_changed("mosaic", None, None)

    assert canvas.tool_controller.get_tool("mosaic") is None
    assert canvas.tool_controller.current_tool is before
    assert canvas.is_editing is False
    assert parent._ocr_mgr.mode is False


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


def test_pixelated_cache_invalidates_on_background_change_and_release(qapp):
    scene = CanvasScene(_gradient_image(), QRectF(0, 0, 64, 64), enable_mosaic=True)
    original = scene.background.pixelated_image(8)
    replacement = QImage(64, 64, QImage.Format.Format_ARGB32)
    replacement.fill(QColor("red"))

    scene.background.update_image(replacement)
    updated = scene.background.pixelated_image(8)
    scene.background.release_image_cache()
    rebuilt = scene.background.pixelated_image(8)

    assert updated != original
    assert updated.pixelColor(16, 16) == QColor("red")
    assert rebuilt == updated


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
    assert cloned.patch_image() == source_item.patch_image()
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
