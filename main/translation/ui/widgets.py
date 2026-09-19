# -*- coding: utf-8 -*-
"""翻译界面的共享小组件。"""

from PySide6.QtCore import QObject, Qt, QTimer

from core.i18n import make_tr

_tr = make_tr("TranslationPopup")


class EllipsisAnimator(QObject):
    """「翻译中…」后面循环的那几个点。

    抽出来是因为翻译有两个入口——快捷键小窗口和大对话框——而这个动画以前
    只长在小窗口上，大窗口一直是一行静止的文字。两条路径各写各的，谁也不知道
    对方长什么样。再抄一份的话，下次有第三个入口就是第三份。

    做成 QObject 而不是控件，是因为两个宿主的文本落点不同（一个是
    result_edit、一个是 target_edit），但都通过 setPlainText 写。
    共享的是「点怎么转」，不是「往哪写」——后者交给回调。
    """

    INTERVAL_MS = 320
    # 0~3 个点循环
    STEPS = 4

    def __init__(self, parent=None, on_tick=None):
        super().__init__(parent)
        self._on_tick = on_tick
        self._step = 0
        self._timer = QTimer(self)
        self._timer.setInterval(self.INTERVAL_MS)
        self._timer.setTimerType(Qt.TimerType.CoarseTimer)
        self._timer.timeout.connect(self._advance)

    def set_tick_callback(self, callback) -> None:
        """回调是宿主的方法，构造时还不存在，所以单独设。"""
        self._on_tick = callback

    def start(self) -> None:
        self._step = 0
        self._timer.start()

    def stop(self) -> None:
        self._timer.stop()
        self._step = 0

    def is_running(self) -> bool:
        return self._timer.isActive()

    @staticmethod
    def text_for(step: int) -> str:
        """第 step 帧该显示的文字。

        先把译好的「翻译中…」尾部的点去掉再补，是因为各语言的省略号不一样
        （中文用…、英文用...），直接往后加会拼成「翻译中….」。
        """
        base = _tr("Translating...").rstrip(".。…")
        return base + "." * (step % EllipsisAnimator.STEPS)

    def _advance(self) -> None:
        self._step = (self._step + 1) % self.STEPS
        if callable(self._on_tick):
            self._on_tick(self.text_for(self._step))
