"""The shared input hub: event routing, the consumer thread and its shutdown."""

import queue
import threading
import time

import pytest
from PySide6.QtCore import QObject, Qt, QThread, Slot

from core import input_hub as hub_module
from core.input_hub import InputHub


class QueueNative:
    """Hub stand-in whose events come from a queue; close() ends next_event like the real Hub."""

    def __init__(self):
        self.events = queue.Queue()
        self.closed = threading.Event()

    def next_event(self, timeout_ms=None):
        return self.events.get()

    def close(self):
        self.closed.set()
        self.events.put(None)


class Receiver(QObject):
    def __init__(self, hub):
        super().__init__()
        self.received, self.threads = [], []
        queued = Qt.ConnectionType.QueuedConnection
        hub.gesture.connect(self.on_gesture, queued)
        hub.moved.connect(self.on_moved, queued)
        hub.failure.connect(self.on_failure, queued)

    def record(self, *event):
        self.received.append(event)
        self.threads.append(QThread.currentThread())

    @Slot(str, int, int, int)
    def on_gesture(self, kind, gesture_id, x, y):
        self.record("gesture", kind, gesture_id, x, y)

    @Slot(int)
    def on_moved(self, gesture_id):
        self.record("moved", gesture_id)

    @Slot(str)
    def on_failure(self, message):
        self.record("failure", message)


EVENTS = [("gesture", "start", 1, -5, 7), ("moved", 1), ("gesture", "finish", 1, 30, 40), ("failure", "boom")]


def test_dispatch_routes_every_kind_to_its_signal(qapp):
    hub = InputHub(QueueNative())
    receiver = Receiver(hub)
    others = []
    hub.side_button.connect(lambda *args: others.append(("side", *args)))
    hub.hotkey.connect(lambda *args: others.append(("hotkey", *args)))
    hub.wheel.connect(lambda *args: others.append(("wheel", *args)))
    hub.key.connect(lambda *args: others.append(("key", *args)))
    hub.foreground.connect(lambda *args: others.append(("foreground", *args)))
    wide_hwnd = 0x7FFF_FFFF_1234
    extra = [("side", "x1"), ("hotkey", "clipboard"), ("wheel", "w", -5, 7, -240, True), ("key", "k", 0x10, True), ("foreground", wide_hwnd)]
    for event in [*EVENTS, *extra, ("unknown", 1)]:
        hub.dispatch(event)
    qapp.processEvents()
    assert receiver.received == EVENTS
    assert others == extra


def test_consumer_thread_delivers_in_order_on_the_gui_thread_and_close_joins(qapp, qtbot):
    native = QueueNative()
    hub = InputHub(native)
    receiver = Receiver(hub)
    hub.start()
    for event in EVENTS:
        native.events.put(event)
    qtbot.waitUntil(lambda: len(receiver.received) == len(EVENTS), timeout=2000)
    assert receiver.received == EVENTS
    assert set(receiver.threads) == {qapp.thread()}
    thread = hub._thread
    hub.close()
    hub.close()
    assert native.closed.is_set()
    assert not thread.is_alive()


def test_a_bad_event_is_logged_and_the_consumer_keeps_running(qapp, qtbot, monkeypatch):
    logged = []
    monkeypatch.setattr(hub_module, "log_exception", lambda exc, context: logged.append(context))
    native = QueueNative()
    hub = InputHub(native)
    receiver = Receiver(hub)
    hub.start()
    native.events.put(("gesture", "start"))
    native.events.put(("moved", 3))
    qtbot.waitUntil(lambda: receiver.received == [("moved", 3)], timeout=2000)
    assert logged == ["InputHub"]
    hub.close()


@pytest.mark.real_input_hub
def test_real_hub_starts_without_low_level_hooks_and_closes_promptly(qapp):
    pytest.importorskip("inputhub")
    assert hub_module._hub is None
    hub = hub_module.input_hub()
    try:
        assert hub_module.input_hub() is hub
        assert not hub.native.stats()["hooks_installed"]
        thread = hub._thread
        assert thread.is_alive()
    finally:
        started = time.perf_counter()
        hub_module.close_input_hub()
    assert time.perf_counter() - started < 1
    assert not thread.is_alive()
    assert hub_module._hub is None
    # Only one native hub may exist per process; closing must free the slot.
    hub_module.close_input_hub()
    hub_module.input_hub()
    hub_module.close_input_hub()
