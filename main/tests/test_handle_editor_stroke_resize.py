# -*- coding: utf-8 -*-
"""LayerEditor 对 StrokeItem（画笔/荧光笔笔迹）拖角缩放的回归测试。

_apply_stroke_item_drag 每次都用 setTransform() 整体替换图元的变换。第二次
（及以后）缩放时，图元身上已经带着上一次缩放留下的 transform——如果新算出的
变换不把它复合进去，而是直接顶替，上一次的缩放结果就会被整个丢弃。
"""
import pytest
from PySide6.QtCore import QPointF
from PySide6.QtGui import QColor, QPainterPath, QPen
from PySide6.QtWidgets import QGraphicsScene

from canvas.handle_editor import HandleType, LayerEditor
from canvas.items import StrokeItem


def _br_handle(editor):
    return next(h for h in editor.handles if h.handle_type == HandleType.CORNER_BR)


def _make_stroke_item():
    """返回 (item, scene)：scene 必须由调用方一直持有到用例结束——它是 item 的
    C++ 宿主，Python 侧的 scene 变量一旦被回收，item 会跟着被销毁。
    """
    path = QPainterPath(QPointF(0, 0))
    path.lineTo(QPointF(100, 50))
    item = StrokeItem(path, QPen(QColor("black"), 4))
    scene = QGraphicsScene(0, 0, 800, 600)
    scene.addItem(item)
    return item, scene


def test_second_resize_of_a_stroke_item_compounds_instead_of_resetting(qapp):
    item, _scene = _make_stroke_item()
    editor = LayerEditor()
    editor.start_edit(item)
    original_width = item.sceneBoundingRect().width()

    # 第一次拖拽：往右下方拖，明显放大
    handle = _br_handle(editor)
    start = QPointF(handle.position)
    editor.start_drag(handle, start)
    editor.drag_to(QPointF(start.x() + 100, start.y() + 50))
    editor.end_drag()
    after_first = item.sceneBoundingRect().width()
    assert after_first > original_width * 1.5, "第一次缩放本身就没放大，用例前提不成立"

    # 第二次拖拽：重新进入编辑、抓住（新位置的）同一个手柄，再往外拖一点
    editor.start_edit(item)
    handle = _br_handle(editor)
    start = QPointF(handle.position)
    editor.start_drag(handle, start)
    editor.drag_to(QPointF(start.x() + 20, start.y() + 10))
    editor.end_drag()
    after_second = item.sceneBoundingRect().width()

    # 关键断言：第二次必须在第一次的结果上继续变大。之前的 bug 会让它弹回到
    # 接近原始大小（因为第二次算出的变换直接顶替、而不是复合第一次的变换）。
    assert after_second > after_first


def test_second_resize_lands_on_the_expected_absolute_size(qapp):
    """更严格的一版：不只看"变大了"，还核对第二次落点的具体宽度。

    两次都往右下拖到手柄的绝对新位置，第二次的宽度应该约等于
    "原始宽度 + 两次位移之和"，而不是"原始宽度 + 只算第二次位移"。
    """
    item, _scene = _make_stroke_item()
    editor = LayerEditor()
    editor.start_edit(item)
    original_width = item.sceneBoundingRect().width()

    handle = _br_handle(editor)
    start = QPointF(handle.position)
    editor.start_drag(handle, start)
    editor.drag_to(QPointF(start.x() + 100, start.y() + 50))
    editor.end_drag()

    editor.start_edit(item)
    handle = _br_handle(editor)
    start = QPointF(handle.position)
    editor.start_drag(handle, start)
    editor.drag_to(QPointF(start.x() + 40, start.y() + 20))
    editor.end_drag()

    after_second = item.sceneBoundingRect().width()
    # 两次拖拽合计把右边缘往外推了 100 + 40 = 140，宽度理应跟着涨这么多
    # （笔迹本身不是纯矩形，容许一点点描边/斜率带来的误差）。
    assert after_second == pytest.approx(original_width + 140, abs=5)
