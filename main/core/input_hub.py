"""进程内共用的全局输入。

低层鼠标键盘钩子和它们的状态机都在 inputhub 的原生线程里，钩子回调当场决定吞不吞，
不进 Python，主线程再忙也不会拖住系统的鼠标键盘。需要界面处理的事件由一个取事件线程
转成本对象的信号；接收方用 QueuedConnection，在自己的线程里处理。
"""

import threading

from PySide6.QtCore import QObject, Signal

from core.logger import log_exception


class InputHub(QObject):
    """包一个 inputhub.Hub（测试时换成同接口的替身），把它的事件转成信号。"""

    gesture = Signal(str, int, int, int)  # start / finish / cancel，手势编号，物理像素坐标
    moved = Signal(int)  # 手势编号；take_position 取走之前只发一次
    side_button = Signal(str)  # x1 / x2
    hotkey = Signal(str)  # bind_hotkey 时给的名字
    wheel = Signal(str, int, int, int, bool)  # 订阅名，物理像素坐标，滚动量（WHEEL_DELTA 倍数），是否横向
    key = Signal(str, int, bool)  # 订阅名，虚拟键码，是否按下
    foreground = Signal(object)  # 窗口句柄；64 位句柄不能走 int 信号
    failure = Signal(str)

    def __init__(self, native, parent=None):
        super().__init__(parent)
        self.native = native
        self._thread = None
        self._closed = False

    def start(self):
        self._thread = threading.Thread(target=self._run, name="InputHubEvents", daemon=True)
        self._thread.start()

    def _run(self):
        # 关闭后 next_event 先交完已排队的事件，再返回 None
        while (event := self.native.next_event()) is not None:
            try:
                self.dispatch(event)
            except Exception as exc:
                log_exception(exc, "InputHub")

    def dispatch(self, event):
        kind = event[0]
        if kind == "gesture":
            self.gesture.emit(*event[1:])
        elif kind == "moved":
            self.moved.emit(event[1])
        elif kind == "side":
            self.side_button.emit(event[1])
        elif kind == "hotkey":
            self.hotkey.emit(event[1])
        elif kind == "wheel":
            self.wheel.emit(*event[1:])
        elif kind == "key":
            self.key.emit(*event[1:])
        elif kind == "foreground":
            self.foreground.emit(event[1])
        elif kind == "failure":
            self.failure.emit(event[1])

    def close(self):
        """卸掉钩子、结束原生线程和取事件线程；之后不再发信号。"""
        if self._closed:
            return
        self._closed = True
        self.native.close()
        if self._thread is not None:
            self._thread.join()
            self._thread = None


_hub = None


def input_hub() -> InputHub:
    """首次调用时创建；只在 GUI 线程调用。低层钩子要等有功能用到时才装。"""
    global _hub
    if _hub is None:
        _hub = _create()
    return _hub


def existing_input_hub():
    """已经创建的输入中心；还没人用过时返回 None，不会因此创建。"""
    return _hub


def _create() -> InputHub:
    import inputhub

    hub = InputHub(inputhub.Hub())
    hub.start()
    return hub


def close_input_hub():
    global _hub
    if _hub is not None:
        _hub.close()
        _hub = None
