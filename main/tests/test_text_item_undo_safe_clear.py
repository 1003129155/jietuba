# -*- coding: utf-8 -*-
"""TextItem 失焦自动删除接入撤销系统的回归测试。

两种失焦触发"删除自己"的场景，处理方式必须不一样：

- 新建文字、还没打过字就点别处：什么都没真的发生过，不该在撤销栈上留痕，
  也不该让用户后续一次 Ctrl+Z 被平白吃掉。
- 已经提交过真实内容的标注，被重新编辑、删空再点别处：这是一次真实的删除，
  必须能用 Ctrl+Z 找回来——连同清空前的文字内容一起，不能只是把一个空壳
  图元加回场景。

这两种行为都依赖 QGraphicsScene 的焦点机制真正跑起来（focusInEvent /
focusOutEvent），而这需要一个"活跃"的 scene——没有 CanvasView 显示出来，
setFocus()/clearFocus() 会静默失效。所以这里都构造并 show() 一个真实
CanvasView（offscreen 平台下安全、不会弹出真实窗口）。
"""
import pytest
from PySide6.QtCore import QPoint, QPointF, QRectF, QSize, Qt
from PySide6.QtGui import QColor, QFont, QImage
from PySide6.QtWidgets import QWidget

from canvas.scene import CanvasScene
from canvas.view import CanvasView
from canvas.items import TextItem


def _image(w=200, h=200):
    img = QImage(w, h, QImage.Format.Format_ARGB32)
    img.fill(QColor("white"))
    return img


@pytest.fixture
def track():
    """登记本用例建的 scene / 窗口，结束时 deleteLater，由 conftest 在用例间冲掉。

    view、scene、PinCanvas 及其父窗口彼此成环，不显式回收就只能等循环 GC
    在后续用例的任意时刻析构它们，这会写坏堆、让后面的 Qt 用例随机崩溃。
    """
    objects = []
    yield objects.append
    for obj in reversed(objects):
        if isinstance(obj, QWidget):
            obj.close()
        obj.deleteLater()


def _active_scene(qapp, track):
    """返回 (scene, view)：调用方必须把两者都一直持有到用例结束。

    QGraphicsItem.setFocus()/clearFocus() 只在 scene "活跃"（isActive()）时
    才会真正触发 focusIn/focusOutEvent；只 show() 还不够——同一个 QApplication
    里如果上一个用例的窗口还没让出"活跃窗口"，这里的 scene 会拿不到焦点，
    这里的操作会静默地什么都不做。显式 activateWindow() 把它抢过来。
    """
    scene = CanvasScene(_image(), QRectF(0, 0, 200, 200))
    track(scene)
    view = CanvasView(scene)
    track(view)
    view.show()
    view.activateWindow()
    qapp.processEvents()
    assert scene.isActive(), "scene 没有真正激活，后面的 focus 断言会全部静默失败"
    return scene, view


def _enter_edit(item, qapp):
    """模拟真实的双击进入编辑：mouseDoubleClickEvent 的等价物。"""
    item.setTextInteractionFlags(Qt.TextInteractionFlag.TextEditorInteraction)
    item.setFocus()
    qapp.processEvents()


def _leave_edit(item, qapp):
    item.clearFocus()
    qapp.processEvents()


def test_creating_text_and_clicking_away_without_typing_leaves_no_undo_trace(qapp, track):
    scene, view = _active_scene(qapp, track)
    # provisional=True 和 TextTool 创建时一致：创建者不入栈，交给失焦结算。
    item = TextItem("", QPointF(50, 50), QFont("Arial", 16), QColor("black"), provisional=True)
    scene.addItem(item)

    _enter_edit(item, qapp)
    before = scene.undo_stack.count()
    _leave_edit(item, qapp)

    assert item.scene() is None, "空文字应该被移除"
    assert scene.undo_stack.count() == before, "不该在撤销栈上留下任何记录"
    assert scene.undo_stack.canUndo() is False


def test_clearing_a_committed_text_item_is_undoable_with_its_content(qapp, track):
    scene, view = _active_scene(qapp, track)
    item = TextItem("", QPointF(50, 50), QFont("Arial", 16), QColor("black"), provisional=True)
    scene.addItem(item)

    # 第一次编辑：打字、确认——内容第一次变得非空，这才是它真正被创建出来的时刻。
    _enter_edit(item, qapp)
    item.setPlainText("hello")
    _leave_edit(item, qapp)
    assert item.scene() is scene
    assert scene.undo_stack.count() == 1

    # 第二次编辑：清空、确认。
    _enter_edit(item, qapp)
    item.setPlainText("")
    _leave_edit(item, qapp)

    assert item.scene() is None, "清空后应该从画布上消失"
    assert scene.undo_stack.canUndo() is True

    scene.undo_stack.undo()
    assert item.scene() is scene, "Ctrl+Z 应该把标注重新放回画布"
    assert item.toPlainText() == "hello", "找回来的必须是清空前的真实内容，不能是空壳"

    scene.undo_stack.redo()
    assert item.scene() is None
    assert item.toPlainText() == ""


def test_pin_canvas_clone_of_a_committed_text_item_stays_undo_safe(qapp, track):
    """钉图克隆的文字标注本来就是真实内容，不是刚创建的空壳。

    initialize_from_items 会把克隆图元推进钉图自己的撤销栈。克隆路径不需要、
    也不应该为此做任何额外标记——TextItem 默认就不是临时文字；一旦它被当成
    临时文字，编辑清空会 (a) 绕开撤销栈直接删除，或 (b) 下次确认非空内容时
    重复推一条 AddItemCommand，和已有的那条打架。
    """
    from pin.pin_canvas import PinCanvas

    source_scene = CanvasScene(_image(), QRectF(0, 0, 200, 200))
    track(source_scene)
    src_item = TextItem("pinned text", QPointF(20, 20), QFont("Arial", 16), QColor("black"))
    source_scene.addItem(src_item)

    parent = QWidget()
    track(parent)
    parent._is_editing = False
    parent.toolbar = None
    pin = PinCanvas(parent, QSize(200, 200), _image())
    track(pin.scene)
    pin_view = CanvasView(pin.scene)
    track(pin_view)
    pin_view.show()
    pin_view.activateWindow()
    qapp.processEvents()

    pin.initialize_from_items([src_item], QPoint(0, 0))
    cloned = next(i for i in pin.scene.items() if isinstance(i, TextItem))

    assert cloned.toPlainText() == "pinned text"
    after_clone = pin.undo_stack.count()

    _enter_edit(cloned, qapp)
    cloned.setPlainText("")
    _leave_edit(cloned, qapp)

    assert cloned.scene() is None
    # 只应该多出一条命令（清空这一步），不该有重复的 AddItemCommand。
    assert pin.undo_stack.count() == after_clone + 1

    pin.undo_stack.undo()
    assert cloned.scene() is pin.scene
    assert cloned.toPlainText() == "pinned text"
