"""Shortcut receivers can be released while Qt still has queued input events."""

import ctypes
import gc
import weakref
from ctypes import wintypes
from unittest.mock import Mock

from core.input_hub import input_hub
from core.shortcut_manager import ShortcutManager, WM_HOTKEY


def test_pending_side_input_does_not_keep_manager_alive_until_cycle_gc(qapp):
    # The application filter also sees queued signal deliveries and destruction.
    ShortcutManager.instance()
    manager = ShortcutManager()
    manager.begin_mouse_capture()
    native = input_hub().native
    assert native.mouse("down", button="x1")
    assert native.mouse("up", button="x1")
    manager.end_mouse_capture()
    reference = weakref.ref(manager)
    was_enabled = gc.isenabled()
    gc.disable()
    try:
        del manager
        assert reference() is None
        qapp.processEvents()
    finally:
        if was_enabled:
            gc.enable()


def test_retained_native_filter_ignores_hotkeys_after_manager_is_gone(qapp):
    manager = ShortcutManager()
    callback = Mock()
    manager._id_to_callback[1] = callback
    event_filter = manager._native_filter
    reference = weakref.ref(manager)
    del manager
    assert reference() is None
    message = wintypes.MSG()
    message.message = WM_HOTKEY
    message.wParam = 1
    assert event_filter.nativeEventFilter(
        b"windows_generic_MSG", ctypes.addressof(message)
    ) == (False, 0)
    callback.assert_not_called()
