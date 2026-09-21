# -*- coding: utf-8 -*-
"""
智能选区在窗口、控件之间跳转时的补间。
"""

from PySide6.QtCore import QEasingCurve, QElapsedTimer, QObject, QRectF, Qt, QTimer


class SmartSelectionAnimator(QObject):
    """把智能选区从当前矩形补间到目标矩形。

    只服务于 hover 预览。hover 的选区是整块窗口矩形或其中的一个控件，跳转时位置
    和尺寸同时变，直接赋值在视觉上是闪现，眼睛要重新找边框在哪。窗口和控件之间
    不分路径：该不该补间由下面的 MIN_TRAVEL_PX 按位移判定。

    补间本身只改矩形，每帧的真实开销来自 selection_model.rectChanged 驱动的重绘——
    遮罩脏区是新旧选区的并集，跨屏幕的大跳意味着每帧接近全屏的半透明填充。
    所以时长按"短到还没来得及反应"取：掉帧时宁可少几帧，也不把动画拖长挡在
    "移到窗口上立刻点"这个动作前面。
    """

    DURATION_MS = 90
    FRAME_MS = 16
    # 四角总位移小于这个值就直接到位：补间看不出来，白付一轮重绘。量的是四个角
    # 的位移之和，所以挡住的是同一块区域内的微调；相邻控件之间（一个 30px 宽的
    # 按钮左右两边各挪 30，合计 60）仍然走补间。
    MIN_TRAVEL_PX = 40

    def __init__(self, apply_rect, parent=None):
        super().__init__(parent)
        self._apply_rect = apply_rect
        self._from = QRectF()
        self._to = QRectF()
        self._elapsed = QElapsedTimer()
        self._curve = QEasingCurve(QEasingCurve.Type.OutCubic)
        self._timer = QTimer(self)
        self._timer.setTimerType(Qt.TimerType.PreciseTimer)
        self._timer.timeout.connect(self._tick)

    @property
    def is_running(self) -> bool:
        return self._timer.isActive()

    @property
    def target(self) -> QRectF:
        """当前补间终点；没在跑时为空矩形。"""
        return QRectF(self._to)

    def animate_to(self, current: QRectF, target: QRectF):
        """从 current 补间到 target。"""
        if target.isEmpty():
            self.stop()
            return

        # hover 每次鼠标移动都会送一次目标，同一个窗口内目标不变。不拦住的话
        # 每帧都会重置起点和计时，补间永远走不完，看起来像选区在慢慢爬。
        if self._timer.isActive() and target == self._to:
            return

        if current.isEmpty() or self._travel(current, target) < self.MIN_TRAVEL_PX:
            self.snap_to(target)
            return

        # 途中换目标就从当前插值位置重新起步，不排队：快速划过一排窗口时
        # 排队会让选区在鼠标停下之后还在追。
        self._from = QRectF(current)
        self._to = QRectF(target)
        self._elapsed.restart()
        if not self._timer.isActive():
            self._timer.start(self.FRAME_MS)

    def snap_to(self, target: QRectF):
        """立即到位并中断补间。"""
        self.stop()
        if not target.isEmpty():
            self._apply_rect(QRectF(target))

    def stop(self):
        self._timer.stop()
        self._from = QRectF()
        self._to = QRectF()

    def _tick(self):
        # 进度按真实耗时算而不是累加帧数：掉帧时动画时长不变，只是帧数变少。
        # 按帧数递增的话，低配机上会变成慢动作，那才是用户眼里的"卡"。
        progress = min(1.0, self._elapsed.elapsed() / float(self.DURATION_MS))
        target = QRectF(self._to)
        if progress >= 1.0:
            self.stop()
            self._apply_rect(target)
            return
        self._apply_rect(self._lerp(self._from, target, self._curve.valueForProgress(progress)))

    @staticmethod
    def _lerp(a: QRectF, b: QRectF, t: float) -> QRectF:
        return QRectF(
            a.x() + (b.x() - a.x()) * t,
            a.y() + (b.y() - a.y()) * t,
            a.width() + (b.width() - a.width()) * t,
            a.height() + (b.height() - a.height()) * t,
        )

    @staticmethod
    def _travel(a: QRectF, b: QRectF) -> float:
        return (abs(a.x() - b.x()) + abs(a.y() - b.y())
                + abs(a.right() - b.right()) + abs(a.bottom() - b.bottom()))
