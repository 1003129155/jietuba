"""全局鼠标快捷键的输入：按住修饰键拖动鼠标。

钩子和手势状态机在共用的输入中心里原生运行，这里只把它们接到截图控制器上。
接收方必须用 Qt.QueuedConnection 连接公开信号，并通过 take_position 取 moved 对应的位置。
"""

from PySide6.QtCore import QObject, Qt, Signal

from core.input_hub import input_hub


class QuickCaptureInput(QObject):
    """有状态的全局拖动输入；公开信号用 QueuedConnection 连接。"""

    event = Signal(str, int, int, int)  # kind, gesture_id, 物理像素坐标 x/y
    moved = Signal(int)  # gesture_id；每个手势至多一个未取走的通知
    failure = Signal(str)

    def __init__(self, parent=None, hub=None):
        super().__init__(parent)
        self._hub = hub if hub is not None else input_hub()
        self._native = self._hub.native
        self._closed = False
        # 在取事件线程上直接转发，接收方只经过自己那一次排队
        self._forwards = ((self._hub.gesture, self.event), (self._hub.moved, self.moved),
                          (self._hub.failure, self.failure))
        for source, target in self._forwards:
            source.connect(target, Qt.ConnectionType.DirectConnection)

    def take_position(self, gesture_id: int):
        """取走最新位置；较新手势的通知不受影响。"""
        return self._native.take_position(gesture_id)

    @property
    def dragging(self):
        return self._native.dragging

    def accepts(self, gesture_id: int) -> bool:
        """排队送达的 start/finish 是否仍然有效；cancel 总是照收。

        重新配置、取消和关闭会让已经结束、但 finish 还没送到界面的手势也作废。
        """
        return self._native.accepts(gesture_id)

    def gesture_binding(self, gesture_id: int):
        """仅限当前手势的 (修饰键集合, 按键)；新手势可能在旧手势的 start 送达前就开始了。"""
        binding = self._native.gesture_binding(gesture_id)
        return None if binding is None else (frozenset(binding[0]), binding[1])

    def configure(self, bindings, enabled: bool):
        """每项为 (修饰键集合, 按键)；按住的修饰键要与集合完全一致。"""
        if self._closed:
            return
        requested = [(sorted(modifiers), button) for modifiers, button in bindings]
        if not self._native.configure_gestures(requested, bool(enabled)) and enabled:
            self.failure.emit("Quick capture requires one or two modifiers")

    def cancel(self):
        self._native.cancel_gesture()

    def set_blocked(self, blocked: bool):
        """由界面决定何时不可用；钩子自己不查窗口和模态状态。"""
        self._native.set_blocked(bool(blocked))

    def close(self):
        """停用手势。已被吞掉的按下仍会等到配对的抬起再放开钩子。"""
        if self._closed:
            return
        self._closed = True
        self._native.configure_gestures([], False)
        for source, target in self._forwards:
            source.disconnect(target)
