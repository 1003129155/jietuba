# -*- coding: utf-8 -*-
"""钉图继承标注时，撤销顺序跟截图窗口里的添加先后一致。"""
from PySide6.QtCore import QPoint, QPointF, QRectF, QSize
from PySide6.QtGui import QColor, QFont, QImage, QPen
from PySide6.QtWidgets import QWidget

from canvas.items import RectItem, TextItem
from canvas.scene import CanvasScene
from canvas.undo import AddItemCommand, RemoveItemCommand


def _image(w=200, h=200):
    img = QImage(w, h, QImage.Format.Format_ARGB32)
    img.fill(QColor("white"))
    return img


def _text(text="note"):
    return TextItem(text, QPointF(20, 20), QFont("Arial", 16), QColor("black"))


def _rect(x=60):
    return RectItem(QRectF(x, 60, 40, 30), QPen(QColor("red"), 3))


def _pin(qapp, drawing_items):
    from pin.pin_canvas import PinCanvas

    parent = QWidget()
    parent._is_editing = False
    parent.toolbar = None
    pin = PinCanvas(parent, QSize(200, 200), _image())
    pin.initialize_from_items(drawing_items, QPoint(0, 0))
    return parent, pin


def _cleanup(qapp, *objects):
    for obj in objects:
        obj.deleteLater()
    qapp.processEvents()


def test_drawing_items_follow_the_undo_stack_not_the_stacking_order(qapp):
    scene = CanvasScene(_image(), QRectF(0, 0, 200, 200))
    text, rect = _text(), _rect()
    # 文字默认总在最上层，叠放层级比后画的矩形高
    assert text.zValue() > rect.zValue()
    scene.undo_stack.push(AddItemCommand(scene, text))
    scene.undo_stack.push(AddItemCommand(scene, rect))

    assert scene.get_drawing_items_in_rect(QRectF(0, 0, 200, 200)) == [text, rect]
    _cleanup(qapp, scene)


def test_restored_item_keeps_its_original_place(qapp):
    scene = CanvasScene(_image(), QRectF(0, 0, 200, 200))
    first, second = _rect(10), _rect(110)
    scene.undo_stack.push(AddItemCommand(scene, first))
    scene.undo_stack.push(AddItemCommand(scene, second))
    scene.undo_stack.push(RemoveItemCommand(scene, first))
    scene.undo_stack.undo()

    assert scene.get_drawing_items_in_rect(QRectF(0, 0, 200, 200)) == [first, second]
    _cleanup(qapp, scene)


def test_pin_undoes_the_last_added_item_first(qapp):
    scene = CanvasScene(_image(), QRectF(0, 0, 200, 200))
    scene.undo_stack.push(AddItemCommand(scene, _text("pinned")))
    scene.undo_stack.push(AddItemCommand(scene, _rect()))

    parent, pin = _pin(qapp, scene.get_drawing_items_in_rect(QRectF(0, 0, 200, 200)))
    [cloned_text] = [i for i in pin.scene.items() if isinstance(i, TextItem)]
    [cloned_rect] = [i for i in pin.scene.items() if type(i) is RectItem]
    # 叠放层级照旧：文字仍压在矩形上面
    assert cloned_text.zValue() > cloned_rect.zValue()

    pin.undo_stack.undo()
    assert cloned_rect.scene() is None
    assert cloned_text.scene() is pin.scene
    _cleanup(qapp, pin.scene, parent, scene)
