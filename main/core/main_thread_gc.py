# -*- coding: utf-8 -*-
"""循环垃圾只在主线程回收。

自动回收由恰好在分配对象的线程执行。关掉的钉图、截图会话常留在循环引用里，
其中的 Qt 对象要是在后台线程被回收析构，会随机原生崩溃。所以关掉自动回收，
改由主线程定时按原来的阈值回收。
"""

import gc

from PySide6.QtCore import QObject, QTimer

_CHECK_INTERVAL_MS = 500


class MainThreadGC(QObject):
    """在主线程创建；创建即接管回收，uninstall() 交还给自动回收。"""

    def __init__(self, parent=None):
        super().__init__(parent)
        self._threshold = gc.get_threshold()
        gc.disable()
        self._timer = QTimer(self)
        self._timer.setInterval(_CHECK_INTERVAL_MS)
        self._timer.timeout.connect(self.check)
        self._timer.start()

    def check(self) -> None:
        """阈值和自动回收相同；每次最多回收一轮，全量回收因此至少隔几十秒。"""
        count0, count1, count2 = gc.get_count()
        threshold0, threshold1, threshold2 = self._threshold
        if count0 <= threshold0:
            return
        if count1 > threshold1:
            gc.collect(2 if count2 > threshold2 else 1)
        else:
            gc.collect(0)

    def uninstall(self) -> None:
        self._timer.stop()
        gc.enable()
