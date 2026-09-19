# -*- coding: utf-8 -*-
"""
选区装饰浮层测试

装饰从 QGraphicsScene 搬到了遮罩之上的 QWidget 浮层。这次搬迁建立了两条以前
靠纪律维持、实际上没人保证的不变量，这里把它们钉住：

1. 装饰不会被遮罩压暗 —— 以前边框和手柄跨出选区的那一半会被半透明黑盖住。
2. 装饰不进导出 —— 以前靠每条导出路径记得 hide/render/恢复。

外加一条没被搬走的：SelectionItem 仍留在场景里接收鼠标事件，所以枚举标注图元
时依然要排除它（漏了会被当成标注克隆进钉图，test_spotlight 覆盖了那一侧）。
"""
import pytest
from PySide6.QtCore import QPoint, QRect, QRectF
from PySide6.QtGui import QColor, QImage, QPainter, QRegion
from PySide6.QtWidgets import QWidget

from canvas.scene import CanvasScene
from canvas.view import CanvasView
from ui.mask_overlay import MaskOverlayWidget
from ui.selection_overlay import SelectionOverlayWidget

W, H = 400, 300
SEL = QRectF(120, 90, 160, 120)


@pytest.fixture
def stack(qapp):
    """底层 view + 遮罩 + 选区装饰浮层，层序与 ScreenshotWindow 一致。"""
    host = QWidget()
    host.resize(W, H)
    bg = QImage(W, H, QImage.Format.Format_ARGB32)
    bg.fill(0xFF1E1E1E)
    scene = CanvasScene(bg, QRectF(0, 0, W, H))
    view = CanvasView(scene, host)
    view.setGeometry(0, 0, W, H)
    mask = MaskOverlayWidget(host, scene.selection_model)
    mask.setGeometry(0, 0, W, H)
    overlay = SelectionOverlayWidget(host, scene.selection_item, scene.selection_model)
    overlay.setGeometry(0, 0, W, H)
    overlay.raise_()
    host.show()
    qapp.processEvents()
    yield host, scene, view, mask, overlay
    view.cleanup()
    host.close()


def _compose(host, scene, mask, overlay, qapp):
    """按真实层序叠出最终画面。"""
    qapp.processEvents()
    out = QImage(W, H, QImage.Format.Format_ARGB32_Premultiplied)
    out.fill(0xFF000000)
    painter = QPainter(out)
    scene.render(painter)
    painter.end()
    mask.render(out, QPoint(), QRegion(mask.rect()))
    overlay.render(out, QPoint(), QRegion(overlay.rect()))
    return out


def _theme_name(scene):
    from core.theme import get_theme
    return get_theme().theme_color.name()


def test_border_is_full_width_and_undimmed(stack, qapp):
    """边框整条都是纯主题色，居中跨在边界上，外侧不再被遮罩压暗。"""
    host, scene, view, mask, overlay = stack
    model = scene.selection_model
    model.activate()
    model.set_rect(SEL)
    model.confirm()
    out = _compose(host, scene, mask, overlay, qapp)

    pure = _theme_name(scene)
    width = int(scene.selection_item.BORDER_WIDTH)
    left = int(SEL.left())
    y = int(SEL.top()) + 20  # 避开上边与角手柄

    outer = [QColor(out.pixel(x, y)).name() for x in range(left - width // 2, left)]
    inner = [QColor(out.pixel(x, y)).name() for x in range(left, left + width // 2)]
    assert outer == [pure] * (width // 2), "边框外半侧应为纯主题色，不应被遮罩压暗"
    assert inner == [pure] * (width // 2), "边框内半侧应为纯主题色"


def test_handles_are_undimmed(stack, qapp):
    """手柄骑在边界上，外半圈同样不应被压暗。"""
    host, scene, view, mask, overlay = stack
    model = scene.selection_model
    model.activate()
    model.set_rect(SEL)
    model.confirm()
    out = _compose(host, scene, mask, overlay, qapp)

    # 遮罩把主题色压暗后的颜色；出现即说明装饰画在了遮罩之下
    from core.theme import get_theme
    tc = get_theme().theme_color
    a = mask._mask_color.alphaF()
    dimmed = QColor(round(tc.red() * (1 - a)), round(tc.green() * (1 - a)),
                    round(tc.blue() * (1 - a))).name()

    y = int(SEL.center().y())  # 左中手柄圆心
    band = [QColor(out.pixel(x, y)).name()
            for x in range(int(SEL.left()) - 20, int(SEL.left()))]
    assert dimmed not in band, "手柄外半圈出现了被遮罩压暗的颜色"


def test_selection_chrome_never_reaches_export(stack, qapp):
    """场景渲染不含任何装饰——导出路径因此不需要先 hide 再恢复。"""
    host, scene, view, mask, overlay = stack
    model = scene.selection_model
    model.activate()
    model.set_rect(SEL)
    model.confirm()
    qapp.processEvents()

    img = QImage(int(SEL.width()), int(SEL.height()), QImage.Format.Format_ARGB32)
    img.fill(0)
    painter = QPainter(img)
    scene.render(painter, QRectF(0, 0, SEL.width(), SEL.height()), SEL)
    painter.end()

    pure = _theme_name(scene)
    hits = sum(1 for y in range(img.height()) for x in range(img.width())
               if QColor(img.pixel(x, y)).name() == pure)
    assert hits == 0, "导出图里出现了选区装饰的主题色像素"


def test_item_paint_draws_nothing(stack, qapp):
    """SelectionItem.paint 是空实现；它留在场景里只为命中区。"""
    host, scene, view, mask, overlay = stack
    item = scene.selection_item
    model = scene.selection_model
    model.activate()
    model.set_rect(SEL)
    model.confirm()

    img = QImage(W, H, QImage.Format.Format_ARGB32)
    img.fill(0)
    painter = QPainter(img)
    item.paint(painter, None, None)
    painter.end()
    assert all(img.pixel(x, y) == 0
               for y in range(0, H, 7) for x in range(0, W, 7))

    assert not item.boundingRect().isEmpty(), "命中区不能跟着绘制一起没了"


def test_overlay_rebind_switches_source(stack, qapp):
    """截图窗口跨会话复用：浮层换绑到新 scene 的 item/model。"""
    host, scene, view, mask, overlay = stack
    bg = QImage(W, H, QImage.Format.Format_ARGB32)
    bg.fill(0xFF1E1E1E)
    scene2 = CanvasScene(bg, QRectF(0, 0, W, H))
    overlay.rebind(scene2.selection_item, scene2.selection_model)

    assert overlay._item is scene2.selection_item
    assert scene2.selection_item.repaint_requested == overlay.refresh
    assert scene.selection_item.repaint_requested is None, "旧 item 的回调应断开"


class _UpdateSpy:
    """记录浮层被要求失效的所有矩形。"""

    def __init__(self, overlay):
        self.overlay = overlay
        self.rects = []
        self._real = overlay.update

    def __enter__(self):
        def spy(*args):
            self.rects.append(QRect(args[0]) if args else self.overlay.rect())
            return self._real(*args)

        self.overlay.update = spy
        return self

    def __exit__(self, *exc):
        del self.overlay.update

    def union(self):
        total = QRect()
        for r in self.rects:
            total = total.united(r)
        return total


def _armed(stack, qapp):
    """确认一个选区并跑完一轮事件循环，返回浮层已经画过的区域。"""
    host, scene, view, mask, overlay = stack
    model = scene.selection_model
    model.activate()
    model.set_rect(SEL)
    model.confirm()
    qapp.processEvents()
    painted = QRect(overlay._painted)
    assert not painted.isEmpty(), "前置条件：浮层应已画出装饰"
    return model, overlay, painted


def test_move_invalidates_both_old_and_new_positions(stack, qapp):
    """选区挪走之后，旧位置的装饰像素必须被失效掉，否则就是残影。"""
    model, overlay, before = _armed(stack, qapp)

    with _UpdateSpy(overlay) as spy:
        model.set_rect(SEL.translated(160, 70))
    dirty = spy.union()

    assert dirty.contains(before), f"失效区域 {dirty} 没盖住旧位置 {before}"
    qapp.processEvents()
    assert dirty.contains(overlay._painted), "失效区域没盖住新位置"


def test_clearing_the_selection_erases_the_last_chrome(stack, qapp):
    """选区没了，最后一帧装饰也要被擦掉。"""
    model, overlay, before = _armed(stack, qapp)

    with _UpdateSpy(overlay) as spy:
        model.deactivate()
    assert spy.union().contains(before), "失效区域没盖住最后一帧装饰"

    qapp.processEvents()
    assert overlay._painted.isEmpty(), "选区已清空，浮层不该再记着画过的区域"


def test_a_partial_repaint_keeps_the_old_chrome_area_on_the_books(stack, qapp):
    """只盖住一部分的被动重绘，不能让浮层忘掉装饰的旧位置。

    本层压在遮罩之上，底下失效一小块就会顺带重绘这一块本层，而这种被动重绘
    可能落在"选区已经变了、refresh() 还没轮到"的中间态：它只擦掉这一小块里的
    装饰，其余部分还留在屏幕上。浮层若把这一帧记成"装饰现在在哪"，下一次
    refresh() 的并集就不再盖住剩下那部分——那就是残影。
    """
    model, overlay, before = _armed(stack, qapp)

    # 选区挪走，但不让浮层收到通知，制造那个中间态
    model.blockSignals(True)
    model.set_rect(SEL.translated(180, 80))
    model.blockSignals(False)

    # 只被动重绘旧装饰的左半边
    partial = QRect(before.left(), before.top(), before.width() // 2, before.height())
    assert not QRegion(before).subtracted(QRegion(partial)).isEmpty(), (
        "前置条件：这块被动重绘必须盖不住旧装饰，否则这条用例测不到东西"
    )
    canvas = QImage(W, H, QImage.Format.Format_ARGB32_Premultiplied)
    canvas.fill(0)
    overlay.render(canvas, QPoint(), QRegion(partial))

    with _UpdateSpy(overlay) as spy:
        overlay.refresh()
    assert spy.union().contains(before), (
        f"失效区域 {spy.union()} 没盖住旧装饰 {before}，剩下的像素没人擦"
    )
