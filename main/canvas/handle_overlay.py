"""
手柄浮层 —— 编辑控制点的独立合成层。

为什么手柄不画在 QGraphicsScene 的 drawForeground 里：

控制点锚在图元包围盒的角上并向外凸出，它们不是 QGraphicsItem，不参与 Qt 的
失效核算。放在场景里意味着每个改动手柄的地方都得**猜**手柄能凸多远，再手写
一个外扩过的脏区（历史上就是 `margin = 25` 这个魔数）。猜错一次就是残影，而
且每加一种新手柄都要重新猜一次。

浮层把"猜"换成了"问"：失效范围直接取自 handle.get_rect() 的并集——手柄画在
哪里，就失效到哪里，同一个真相源。

两个反直觉但必须遵守的约束（都是踩出来的）：

1. 浮层必须**永远铺满整个 viewport**，几何不能跟着手柄走。
   跟着手柄走的话，浮层几何就成了 (手柄 × viewportTransform × viewport 尺寸)
   的缓存投影，任何没通知到的 transform 变化（滚轮缩放、外部 setTransform）
   都会让它卡在旧矩形上，把手柄裁掉——比残影更糟，手柄直接消失。
   铺满之后几何只依赖 viewport 尺寸，不可能过期；而且视图变换导致整个 viewport
   重绘时，压在上面的浮层会被一并重绘，缩放自动就对了，一个通知都不需要。

2. 但**不能整层 update()**。浮层是半透明的，整层失效会连带把它底下的内容层
   全部重绘，比原来的整场景重绘还贵（实测两者开销一模一样）。所以一律用
   update(rect)，rect 由手柄自己算出来，并且带上上一次画过的区域好擦干净。

附带的好处：scene.render() 导出时不会再把控制点烤进图片（和遮罩层移出场景
是同一个理由）。
"""

from __future__ import annotations

from PySide6.QtCore import QRect, Qt
from PySide6.QtGui import QPainter
from PySide6.QtWidgets import QWidget

from core import safe_event


class HandleOverlayWidget(QWidget):
    """绘制 LayerEditor 控制点的透明浮层，铺满 viewport，按需局部失效。"""

    # 手柄描边 + 抗锯齿会溢出 get_rect() 一点点
    EDGE_ALLOWANCE = 3

    def __init__(self, view):
        super().__init__(view.viewport())
        self._view = view
        # 上一次画过的区域（viewport 坐标），下次失效时要连它一起擦
        self._painted = QRect()

        # 鼠标事件必须穿透，否则 view 收不到任何交互
        self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
        self.setAttribute(Qt.WidgetAttribute.WA_NoSystemBackground, True)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, True)
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.setMouseTracking(False)

        self.setGeometry(view.viewport().rect())
        self.show()
        self.raise_()

    # ------------------------------------------------------------------
    # 内部
    # ------------------------------------------------------------------
    def _editor(self):
        controller = getattr(self._view, "smart_edit_controller", None)
        return getattr(controller, "layer_editor", None) if controller else None

    def _handles_viewport_rect(self) -> QRect:
        """chrome 在 viewport 坐标下的包围盒；没东西可画时返回空矩形。

        范围由 LayerEditor.visual_bounds() 给出——它和 render() 挨在一起，
        所以浮层不需要知道 chrome 到底画了些什么。
        """
        editor = self._editor()
        if editor is None:
            return QRect()

        bounds = editor.visual_bounds()
        if bounds.isNull():
            return QRect()

        pad = self.EDGE_ALLOWANCE
        rect = self._view.viewportTransform().mapRect(bounds).toAlignedRect()
        return rect.adjusted(-pad, -pad, pad, pad).intersected(self.rect())

    # ------------------------------------------------------------------
    # 外部接口
    # ------------------------------------------------------------------
    def refresh(self):
        """手柄状态一变就调它：失效"新位置 ∪ 旧位置"。"""
        viewport_rect = self._view.viewport().rect()
        if self.geometry() != viewport_rect:
            # viewport 改尺寸了。整块重来，旧痕迹由这次全量失效带走。
            self.setGeometry(viewport_rect)
            self._painted = QRect()
            self.update()
            return

        current = self._handles_viewport_rect()
        dirty = current.united(self._painted)
        if not dirty.isEmpty():
            self.update(dirty)

    @safe_event
    def paintEvent(self, event):
        editor = self._editor()
        if editor is None or not editor.is_editing():
            self._painted = QRect()
            return

        painter = QPainter(self)
        try:
            painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
            # 浮层与 viewport 同尺寸同原点，所以 viewportTransform 直接可用
            painter.setWorldTransform(self._view.viewportTransform())
            editor.render(painter)
        finally:
            painter.end()

        # 记录这一帧手柄实际落在哪里。视图变换导致的被动重绘也会走到这里，
        # 所以 _painted 始终反映屏幕上的真实情况，不依赖任何人来通知。
        self._painted = self._handles_viewport_rect()
