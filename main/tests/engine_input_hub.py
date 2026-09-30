"""测试用的输入中心：状态机是真正的原生实现（inputhub.Engine），不装钩子，输入由测试逐条给出。"""

import weakref

import inputhub

from core.input_hub import InputHub


class EngineHub:
    """inputhub.Hub 的替身。

    产生的事件在调用 mouse/key 的线程上直接交给 InputHub 分发；从别的线程调用时，
    和正式环境的取事件线程一样，接收方要经过一次排队。
    """

    def __init__(self):
        self.engine = inputhub.Engine()
        self.held = set()
        self.hub = None  # 弱引用：InputHub 持有本对象，反过来强引用会成循环

    def _deliver(self, events):
        for event in events:
            self.hub().dispatch(event)

    # ---- 正式代码用到的 Hub 接口

    def configure_gestures(self, bindings, enabled):
        valid, events = self.engine.configure_gestures(bindings, enabled)
        self._deliver(events)
        return valid

    def set_blocked(self, blocked):
        self._deliver(self.engine.set_blocked(blocked))

    def cancel_gesture(self):
        self._deliver(self.engine.cancel())

    def take_position(self, gesture_id):
        return self.engine.take_position(gesture_id)

    @property
    def dragging(self):
        return self.engine.dragging

    def accepts(self, gesture_id):
        return self.engine.accepts(gesture_id)

    def gesture_binding(self, gesture_id):
        return self.engine.gesture_binding(gesture_id)

    def configure_side_buttons(self, enabled, suppressed, capture_all=False):
        self.engine.configure_side_buttons(enabled, suppressed, capture_all)

    def bind_hotkey(self, name, modifiers, vk):
        return self.engine.bind_hotkey(name, modifiers, vk)

    def unbind_hotkey(self, name):
        self.engine.unbind_hotkey(name)

    def watch_wheel(self, watcher, rect=None):
        self.engine.watch_wheel(watcher, rect)

    def unwatch_wheel(self, watcher):
        self.engine.unwatch_wheel(watcher)

    def watch_keys(self, watcher, keys):
        self.engine.watch_keys(watcher, keys)

    def unwatch_keys(self, watcher):
        self.engine.unwatch_keys(watcher)

    def watch_foreground(self, watcher):
        self.engine.watch_foreground(watcher)

    def unwatch_foreground(self, watcher):
        self.engine.unwatch_foreground(watcher)

    def close(self):
        self._deliver(self.engine.close())

    # ---- 测试输入与观察

    @property
    def hooks_needed(self):
        return self.engine.hooks_needed

    @property
    def mask_calls(self):
        return self.engine.mask_calls

    @property
    def foreground_needed(self):
        return self.engine.foreground_needed

    def hold(self, *keys):
        """这些键在装钩子之前就已按着，钩子没见过它们的按下。"""
        self.held.update(keys)
        self.engine.set_held(sorted(self.held))

    def release_all(self):
        self.held.clear()
        self.engine.set_held([])

    def mouse(self, action, x=10, y=20, button=None, delta=0, injected=False):
        suppress, events = self.engine.mouse(action, x, y, button, delta, injected)
        self._deliver(events)
        return suppress

    def key(self, vk, pressed, injected=False):
        # 和真实钩子一样，状态机先看到这次按键，按住的键随后才更新
        suppress, events = self.engine.key(vk, pressed, injected)
        if pressed:
            self.held.add(vk)
        else:
            self.held.discard(vk)
        self._deliver(events)
        return suppress

    def foreground(self, hwnd):
        self._deliver(self.engine.foreground(hwnd))

    def fail(self, message):
        self._deliver(self.engine.fail(message))

    def set_mask_error(self, message):
        self.engine.set_mask_error(message)


def engine_input_hub():
    native = EngineHub()
    hub = InputHub(native)
    native.hub = weakref.ref(hub)
    return hub
