"""Isolated Qt cases invoked one function family per child process."""

import os
import subprocess
import sys
import textwrap
import gc

import pytest

from PySide6.QtCore import QEvent, QPoint, QPointF, QRectF, Qt
from PySide6.QtGui import (
    QColor,
    QImage,
    QInputMethodEvent,
    QPainterPath,
    QPen,
    QWheelEvent,
)
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication, QWidget

from canvas.items import StrokeItem
from canvas.scene import CanvasScene
from canvas.undo import AddItemCommand
from canvas.view import CanvasView


@pytest.fixture(autouse=True)
def _close_test_windows(qapp):
    existing = set(QApplication.topLevelWidgets())
    yield
    created = [
        window for window in QApplication.topLevelWidgets()
        if window not in existing
    ]
    for window in created:
        for view in window.findChildren(CanvasView):
            scene = view.scene()
            view.cleanup()
            view.setScene(None)
            if scene is not None:
                scene.tool_controller = None
                scene.undo_stack.clear()
                scene._layer_editor = None
                scene.deleteLater()
            view.setParent(None)
            view.deleteLater()
        window.close()
        window.deleteLater()
    QApplication.sendPostedEvents(None, QEvent.Type.DeferredDelete)
    qapp.processEvents()


@pytest.fixture(scope="module", autouse=True)
def _pause_cyclic_gc_for_qt_stress_process():
    if gc.isenabled():
        gc.disable()
    yield
    # The dedicated qt_pytest_runner uses os._exit(pytest_code), so this
    # process never re-enters PySide cyclic finalization after pytest returns.


class ScreenshotWindow(QWidget):
    def __init__(self):
        super().__init__()
        self.confirm_calls = 0

    def _handle_confirm(self):
        self.confirm_calls += 1


def _make_view(qapp, *, confirm_on_double_click=True, confirmed=True):
    image = QImage(160, 120, QImage.Format.Format_ARGB32)
    image.fill(QColor("white"))
    scene = CanvasScene(image, QRectF(0, 0, 160, 120))
    if confirmed:
        scene.selection_model.initialize_confirmed_rect(QRectF(0, 0, 160, 120))
    window = ScreenshotWindow()
    window.resize(160, 120)
    view = CanvasView(
        scene,
        window,
        confirm_on_double_click=confirm_on_double_click,
    )
    view.setGeometry(0, 0, 160, 120)
    window.show()
    qapp.processEvents()
    return window, scene, view


def _single_then_double(view, point=QPoint(80, 60), modifiers=Qt.KeyboardModifier.NoModifier):
    # Do not let cyclic-GC destruct stale Qt wrappers re-entrantly from inside
    # a synthetic mouse callback after dozens of view lifecycles in one process.
    restore_gc = gc.isenabled()
    if restore_gc:
        gc.disable()
    try:
        QTest.mouseClick(view.viewport(), Qt.MouseButton.LeftButton, modifiers, point)
        QTest.mouseDClick(view.viewport(), Qt.MouseButton.LeftButton, modifiers, point)
    finally:
        if restore_gc:
            gc.enable()


def test_safe_blank_double_click_confirms_exactly_once(qapp):
    window, _scene, view = _make_view(qapp)

    _single_then_double(view)

    assert window.confirm_calls == 1


def test_pen_first_click_is_rolled_back_before_confirm(qapp):
    window, scene, view = _make_view(qapp)
    scene.activate_tool("pen")

    _single_then_double(view)

    assert window.confirm_calls == 1
    assert scene.undo_stack.index() == 0
    assert not any(isinstance(item, StrokeItem) for item in scene.items())


@pytest.mark.parametrize(
    ("tool_id", "item_type_name"),
    [
        ("highlighter", "StrokeItem"),
        ("arrow", "ArrowItem"),
        ("rect", "RectItem"),
        ("ellipse", "EllipseItem"),
    ],
)
def test_other_drawing_tool_first_click_is_rolled_back_before_confirm(
    qapp, tool_id, item_type_name
):
    from canvas import items as canvas_items

    window, scene, view = _make_view(qapp)
    scene.activate_tool(tool_id)

    _single_then_double(view)

    item_type = getattr(canvas_items, item_type_name)
    assert window.confirm_calls == 1
    assert scene.undo_stack.index() == 0
    assert not any(isinstance(item, item_type) for item in scene.items())


def test_number_first_click_is_rolled_back_before_confirm(qapp):
    from canvas.items import NumberItem

    window, scene, view = _make_view(qapp)
    scene.activate_tool("number")

    _single_then_double(view)

    assert window.confirm_calls == 1
    assert scene.undo_stack.index() == 0
    assert not any(isinstance(item, NumberItem) for item in scene.items())


def test_small_number_handle_does_not_steal_blank_double_click(qapp):
    from canvas.items import NumberItem

    window, scene, view = _make_view(qapp)
    scene.activate_tool("number")
    scene.update_style(width=1)

    _single_then_double(view)

    assert window.confirm_calls == 1
    assert scene.undo_stack.index() == 0
    assert not any(isinstance(item, NumberItem) for item in scene.items())


def test_empty_text_first_click_is_rolled_back_before_confirm(qapp):
    from canvas.items import TextItem

    window, scene, view = _make_view(qapp)
    scene.activate_tool("text")

    _single_then_double(view)

    assert window.confirm_calls == 1
    assert scene.undo_stack.index() == 0
    assert not any(isinstance(item, TextItem) for item in scene.items())


def test_existing_text_edit_double_click_confirms_and_preserves_text(qapp):
    from canvas.items import TextItem
    from PySide6.QtGui import QFont

    window, scene, view = _make_view(qapp)
    item = TextItem("keep", QPointF(60, 45), QFont("Arial", 14), QColor("black"))
    scene.undo_stack.push(AddItemCommand(scene, item))

    _single_then_double(view, QPoint(75, 55))

    assert window.confirm_calls == 1
    assert item.scene() is scene
    assert item.toPlainText() == "keep"


def test_nonempty_provisional_text_cancels_double_click_confirm(qapp):
    from canvas.items import TextItem

    window, scene, view = _make_view(qapp)
    scene.activate_tool("text")
    point = QPoint(80, 60)

    QTest.mouseClick(view.viewport(), Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, point)
    QTest.keyClicks(view.viewport(), "keep")
    QTest.mouseDClick(view.viewport(), Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, point)

    assert window.confirm_calls == 0
    assert any(item.toPlainText() == "keep" for item in scene.items() if isinstance(item, TextItem))


def test_mosaic_first_click_is_rolled_back_before_confirm(qapp):
    from canvas.items import MosaicItem

    image = QImage(160, 120, QImage.Format.Format_ARGB32)
    image.fill(QColor("white"))
    scene = CanvasScene(image, QRectF(0, 0, 160, 120), enable_mosaic=True)
    scene.selection_model.initialize_confirmed_rect(QRectF(0, 0, 160, 120))
    window = ScreenshotWindow()
    view = CanvasView(scene, window, confirm_on_double_click=True)
    view.setGeometry(0, 0, 160, 120)
    window.show()
    qapp.processEvents()
    scene.activate_tool("mosaic")

    _single_then_double(view)

    assert window.confirm_calls == 1
    assert scene.undo_stack.index() == 0
    assert not any(isinstance(item, MosaicItem) for item in scene.items())


def test_existing_annotation_double_click_confirms_without_removing_item(qapp):
    window, scene, view = _make_view(qapp)
    path = QPainterPath(QPointF(70, 60))
    path.lineTo(QPointF(90, 60))
    item = StrokeItem(path, QPen(QColor("red"), 8))
    scene.undo_stack.push(AddItemCommand(scene, item))

    _single_then_double(view)

    assert window.confirm_calls == 1
    assert item.scene() is scene


def test_existing_mosaic_double_click_confirms_without_removing_item(qapp):
    from canvas.items import MosaicItem

    image = QImage(160, 120, QImage.Format.Format_ARGB32)
    image.fill(QColor("white"))
    scene = CanvasScene(image, QRectF(0, 0, 160, 120), enable_mosaic=True)
    scene.selection_model.initialize_confirmed_rect(QRectF(0, 0, 160, 120))
    window = ScreenshotWindow()
    view = CanvasView(scene, window, confirm_on_double_click=True)
    view.setGeometry(0, 0, 160, 120)
    window.show()
    qapp.processEvents()
    path = QPainterPath(QPointF(70, 60))
    path.lineTo(QPointF(90, 60))
    item = MosaicItem(path, 20, 8, image, QPointF(0, 0))
    scene.undo_stack.push(AddItemCommand(scene, item))
    before = (scene.undo_stack.count(), scene.undo_stack.index())

    _single_then_double(view)

    assert window.confirm_calls == 1
    assert item.scene() is scene
    assert (scene.undo_stack.count(), scene.undo_stack.index()) == before


def test_eraser_first_click_is_rolled_back_before_confirm(qapp):
    window, scene, view = _make_view(qapp)
    path = QPainterPath(QPointF(40, 60))
    path.lineTo(QPointF(45, 60))
    item = StrokeItem(path, QPen(QColor("red"), 2))
    scene.undo_stack.push(AddItemCommand(scene, item))
    scene.activate_tool("eraser")
    scene.update_style(width=50)

    QTest.mouseClick(view.viewport(), Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, QPoint(65, 60))

    assert view._double_click_candidate is not None
    assert scene.undo_stack.count() == 2
    assert scene.undo_stack.index() == 2

    QTest.mouseDClick(view.viewport(), Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, QPoint(65, 60))

    assert window.confirm_calls == 1
    assert item.scene() is scene
    assert scene.undo_stack.index() == 1


def test_default_canvas_is_opted_out(qapp):
    window, _scene, view = _make_view(qapp, confirm_on_double_click=False)

    _single_then_double(view)

    assert window.confirm_calls == 0


def _run_isolated_qt_probe(source):
    bootstrap = """
    import sys
    from pathlib import Path
    sys.path.insert(0, str(Path.cwd() / "main"))
    """
    result = subprocess.run(
        [sys.executable, "-c", textwrap.dedent(bootstrap) + textwrap.dedent(source)],
        cwd=str(__import__("pathlib").Path(__file__).parents[2]),
        env=os.environ.copy(),
        capture_output=True,
        text=True,
        timeout=30,
        check=False,
    )
    assert result.returncode == 0, result.stdout + result.stderr


def test_pin_canvas_view_is_opted_out(qapp):
    _run_isolated_qt_probe(
        """
        from types import SimpleNamespace
        from PySide6.QtCore import QRectF
        from PySide6.QtGui import QColor, QImage
        from PySide6.QtWidgets import QApplication
        from canvas.scene import CanvasScene
        from pin.pin_canvas_view import PinCanvasView

        app = QApplication.instance() or QApplication([])
        image = QImage(80, 60, QImage.Format.Format_ARGB32)
        image.fill(QColor("white"))
        scene = CanvasScene(image, QRectF(0, 0, 80, 60))
        scene.selection_model.initialize_confirmed_rect(QRectF(0, 0, 80, 60))
        view = PinCanvasView(scene, SimpleNamespace(), SimpleNamespace(is_editing=True))
        assert view.confirm_on_double_click is False
        view.close()
        app.processEvents()
        """
    )


def test_gif_drawing_view_is_opted_out(qapp):
    _run_isolated_qt_probe(
        """
        from PySide6.QtCore import QRect
        from PySide6.QtWidgets import QApplication
        from core.shortcut_manager import ShortcutManager
        from gif.drawing_view import GifDrawingView

        app = QApplication.instance() or QApplication([])
        view = GifDrawingView(QRect(0, 0, 80, 60))
        assert view.confirm_on_double_click is False
        assert view.scene_obj.tool_controller.get_tool("mosaic") is None
        ShortcutManager.instance().unregister(view._shortcut_handler)
        view.close()
        app.processEvents()
        """
    )


def test_active_drawing_with_redo_branch_rolls_back_and_confirms(qapp):
    window, scene, view = _make_view(qapp)
    path = QPainterPath(QPointF(10, 10))
    path.lineTo(QPointF(20, 20))
    old_item = StrokeItem(path, QPen(QColor("blue"), 3))
    scene.undo_stack.push(AddItemCommand(scene, old_item))
    scene.undo_stack.undo()
    assert scene.undo_stack.canRedo()
    scene.activate_tool("pen")

    _single_then_double(view)

    assert window.confirm_calls == 1
    assert scene.undo_stack.index() == 0
    assert not any(isinstance(item, StrokeItem) for item in scene.items())


@pytest.mark.parametrize("with_redo", [False, True])
def test_merged_number_control_first_click_is_restored_and_confirms(monkeypatch, qapp, with_redo):
    from canvas.items import NumberItem
    from canvas.undo import NumberEditCommand

    window, scene, view = _make_view(qapp)
    item = NumberItem(3, QPointF(80, 60), 20, QColor("red"))
    scene.addItem(item)
    first = NumberEditCommand(item, {"number": 3}, {"number": 4}, next_before=4, next_after=5)
    scene.undo_stack.push(first)
    assert item.number == 4

    if with_redo:
        path = QPainterPath(QPointF(10, 10))
        path.lineTo(QPointF(20, 20))
        redo_item = StrokeItem(path, QPen(QColor("blue"), 3))
        scene.undo_stack.push(AddItemCommand(scene, redo_item))
        scene.undo_stack.undo()
        assert scene.undo_stack.index() == 1
        assert scene.undo_stack.count() == 2

    original_tail = first.capture_merge_tail()

    def merge_number(*_args):
        scene.undo_stack.push(
            NumberEditCommand(item, {"number": 4}, {"number": 5}, next_before=5, next_after=6)
        )
        return True

    monkeypatch.setattr(view.smart_edit_controller, "handle_edit_press", merge_number)

    _single_then_double(view)

    assert window.confirm_calls == 1
    assert item.number == 4
    assert first.merge_tail_matches(original_tail)
    assert scene.undo_stack.index() == 1


def test_real_number_increment_handle_merge_is_restored_and_confirms(qapp):
    from canvas.items import NumberItem
    from canvas.undo import NumberEditCommand

    window, scene, view = _make_view(qapp)
    item = NumberItem(3, QPointF(80, 60), 20, QColor("red"))
    scene.addItem(item)
    first = NumberEditCommand(item, {"number": 3}, {"number": 4}, next_before=4, next_after=5)
    scene.undo_stack.push(first)
    view.smart_edit_controller.select_item(item)
    handle = view.smart_edit_controller.layer_editor.handles[0]
    point = view.mapFromScene(handle.position)

    _single_then_double(view, point)

    assert window.confirm_calls == 1
    assert item.number == 4
    assert scene.undo_stack.index() == 1


def test_merged_number_restore_failure_rejects_confirmation(monkeypatch, qapp):
    from canvas.items import NumberItem
    from canvas.undo import NumberEditCommand
    from tools.number import NumberTool

    window, scene, view = _make_view(qapp)
    item = NumberItem(3, QPointF(80, 60), 20, QColor("red"))
    scene.addItem(item)
    first = NumberEditCommand(item, {"number": 3}, {"number": 4}, next_before=4, next_after=5)
    scene.undo_stack.push(first)
    view.smart_edit_controller.select_item(item)
    handle = view.smart_edit_controller.layer_editor.handles[0]
    point = view.mapFromScene(handle.position)

    QTest.mouseClick(view.viewport(), Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, point)
    assert item.number == 5
    monkeypatch.setattr(NumberTool, "set_next_number_and_refresh", lambda *_args, **_kwargs: 6)
    QTest.mouseDClick(view.viewport(), Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, point)

    assert window.confirm_calls == 0


def test_drag_created_selection_allows_double_click_confirmation(qapp):
    window, scene, view = _make_view(qapp, confirmed=False)
    viewport = view.viewport()

    QTest.mousePress(viewport, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, QPoint(20, 20))
    QTest.mouseMove(viewport, QPoint(140, 100))
    QTest.mouseRelease(viewport, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, QPoint(140, 100))

    assert scene.selection_model.is_confirmed
    assert view.is_dragging_selection is False
    _single_then_double(view, QPoint(80, 60))
    assert window.confirm_calls == 1


def test_modified_double_click_does_not_confirm(qapp):
    window, _scene, view = _make_view(qapp)

    _single_then_double(view, modifiers=Qt.KeyboardModifier.ControlModifier)

    assert window.confirm_calls == 0


def test_second_click_outside_drag_tolerance_does_not_confirm(qapp):
    window, _scene, view = _make_view(qapp)

    QTest.mouseClick(view.viewport(), Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, QPoint(60, 60))
    QTest.mouseDClick(view.viewport(), Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, QPoint(90, 60))

    assert window.confirm_calls == 0


def test_selection_border_double_click_restores_rect_and_confirms(qapp):
    window, _scene, view = _make_view(qapp)
    before = _scene.selection_model.rect()

    _single_then_double(view, point=QPoint(3, 60))

    assert window.confirm_calls == 1
    assert _scene.selection_model.rect() == before


def test_subthreshold_crop_mutation_is_restored_before_confirm(qapp):
    window, scene, view = _make_view(qapp)
    before = scene.selection_model.rect()
    point = QPoint(3, 60)

    QTest.mouseClick(view.viewport(), Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, point)
    scene.selection_model.set_rect(before.adjusted(2, 0, 0, 0))
    QTest.mouseDClick(view.viewport(), Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, point)

    assert window.confirm_calls == 1
    assert scene.selection_model.rect() == before


def test_real_edit_command_first_click_is_undone_before_confirm(qapp):
    from canvas.undo import EditItemCommand

    window, scene, view = _make_view(qapp)
    path = QPainterPath(QPointF(60, 60))
    path.lineTo(QPointF(90, 60))
    item = StrokeItem(path, QPen(QColor("red"), 8))
    scene.addItem(item)
    point = QPoint(75, 60)

    QTest.mouseClick(view.viewport(), Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, point)
    scene.undo_stack.push(EditItemCommand(item, {"pos": QPointF(0, 0)}, {"pos": QPointF(2, 0)}))
    QTest.mouseDClick(view.viewport(), Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, point)

    assert window.confirm_calls == 1
    assert item.pos() == QPointF(0, 0)
    assert scene.undo_stack.index() == 0
    if os.environ.get("JIETUBA_ISOLATED_QT_CASE") == "1":
        sys.stdout.flush()
        sys.stderr.flush()
        os._exit(0)


def test_unknown_multiple_command_delta_fails_closed(qapp):
    window, scene, view = _make_view(qapp)
    point = QPoint(80, 60)
    QTest.mouseClick(view.viewport(), Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, point)
    for offset in (0, 10):
        path = QPainterPath(QPointF(10 + offset, 10))
        path.lineTo(QPointF(15 + offset, 15))
        scene.undo_stack.push(
            AddItemCommand(scene, StrokeItem(path, QPen(QColor("blue"), 2)))
        )

    QTest.mouseDClick(view.viewport(), Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, point)

    assert window.confirm_calls == 0


def test_wheel_and_ime_input_invalidate_candidate(qapp):
    for kind in ("wheel", "ime"):
        window, _scene, view = _make_view(qapp)
        point = QPoint(80, 60)
        QTest.mouseClick(view.viewport(), Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, point)
        if kind == "wheel":
            view.wheelEvent(QWheelEvent(
                QPointF(point), QPointF(point), QPoint(), QPoint(0, 120),
                Qt.MouseButton.NoButton, Qt.KeyboardModifier.NoModifier,
                Qt.ScrollPhase.ScrollUpdate, False,
            ))
        else:
            view.inputMethodEvent(QInputMethodEvent("x", []))
        QTest.mouseDClick(view.viewport(), Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, point)
        assert window.confirm_calls == 0
        window.close()


def test_drag_invalidates_double_click_candidate(qapp):
    window, scene, view = _make_view(qapp)
    scene.activate_tool("pen")
    viewport = view.viewport()
    QTest.mousePress(viewport, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, QPoint(60, 60))
    QTest.mouseMove(viewport, QPoint(90, 60))
    QTest.mouseRelease(viewport, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, QPoint(90, 60))
    QTest.mouseDClick(viewport, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, QPoint(90, 60))

    assert window.confirm_calls == 0


def test_edit_handle_double_click_candidate_keeps_confirm_priority(monkeypatch, qapp):
    window, _scene, view = _make_view(qapp)
    monkeypatch.setattr(view.smart_edit_controller, "handle_edit_press", lambda *_args: True)

    _single_then_double(view)

    assert window.confirm_calls == 1
