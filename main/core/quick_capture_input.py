"""Global modifier + mouse-drag input for quick capture.

Hooks own only input state. Consumers must connect public signals with
Qt.QueuedConnection and consume ``moved`` through ``take_position``.
No desktop capture, Qt widgets or settings access runs on the hook thread.
"""

import ctypes
import threading
from ctypes import wintypes
from functools import lru_cache

from PySide6.QtCore import QObject, Qt, Signal


WM_MOUSEMOVE = 0x0200
WM_LBUTTONDOWN = 0x0201
WM_LBUTTONUP = 0x0202
WM_RBUTTONDOWN = 0x0204
WM_RBUTTONUP = 0x0205
WM_MBUTTONDOWN = 0x0207
WM_MBUTTONUP = 0x0208
WM_MOUSEWHEEL = 0x020A
WM_XBUTTONDOWN = 0x020B
WM_XBUTTONUP = 0x020C
WM_MOUSEHWHEEL = 0x020E
WM_KEYDOWN = 0x0100
WM_KEYUP = 0x0101
WM_SYSKEYDOWN = 0x0104
WM_SYSKEYUP = 0x0105
VK_ESCAPE = 0x1B
_MODIFIER_KEYS = {
    "ctrl": (0xA2, 0xA3),
    "shift": (0xA0, 0xA1),
    "alt": (0xA4, 0xA5),
    "win": (0x5B, 0x5C),
}
_KEY_MODIFIERS = {vk: name for name, keys in _MODIFIER_KEYS.items() for vk in keys}
_KEY_MODIFIERS.update({0x10: "shift", 0x11: "ctrl", 0x12: "alt"})
_BUTTONS = ("left", "middle", "right", "x1", "x2")
_BUTTON_DOWN = {WM_LBUTTONDOWN: "left", WM_RBUTTONDOWN: "right", WM_MBUTTONDOWN: "middle", WM_XBUTTONDOWN: "x"}
_BUTTON_UP = {WM_LBUTTONUP: "left", WM_RBUTTONUP: "right", WM_MBUTTONUP: "middle", WM_XBUTTONUP: "x"}


class _MouseData(ctypes.Structure):
    _fields_ = [("pt", wintypes.POINT), ("mouseData", wintypes.DWORD),
                ("flags", wintypes.DWORD), ("time", wintypes.DWORD),
                ("dwExtraInfo", ctypes.c_size_t)]


class _KeyboardData(ctypes.Structure):
    _fields_ = [("vkCode", wintypes.DWORD), ("scanCode", wintypes.DWORD),
                ("flags", wintypes.DWORD), ("time", wintypes.DWORD),
                ("dwExtraInfo", ctypes.c_size_t)]


class _MouseInput(ctypes.Structure):
    _fields_ = [("dx", wintypes.LONG), ("dy", wintypes.LONG),
                ("mouseData", wintypes.DWORD), ("dwFlags", wintypes.DWORD),
                ("time", wintypes.DWORD), ("dwExtraInfo", ctypes.c_size_t)]


class _KeyboardInput(ctypes.Structure):
    _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD),
                ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD),
                ("dwExtraInfo", ctypes.c_size_t)]


class _InputUnion(ctypes.Union):
    # Including MOUSEINPUT preserves INPUT's native size on x64 and ARM64.
    _fields_ = [("mi", _MouseInput), ("ki", _KeyboardInput)]


class _Input(ctypes.Structure):
    _fields_ = [("type", wintypes.DWORD), ("value", _InputUnion)]


@lru_cache(maxsize=1)
def _user32():
    api = ctypes.WinDLL("user32", use_last_error=True)
    api.GetAsyncKeyState.argtypes = [ctypes.c_int]
    api.GetAsyncKeyState.restype = ctypes.c_short
    api.SendInput.argtypes = [wintypes.UINT, ctypes.POINTER(_Input), ctypes.c_int]
    api.SendInput.restype = wintypes.UINT
    return api


def _current_modifiers(exclude_vk=None):
    api = _user32()
    return frozenset(name for name, keys in _MODIFIER_KEYS.items()
                     if any(vk != exclude_vk and api.GetAsyncKeyState(vk) & 0x8000
                            for vk in keys))


def _button_of(table, message, data):
    button = table.get(message)
    if button == "x":
        # The high word of mouseData tells side buttons apart: XBUTTON1 = 1, XBUTTON2 = 2.
        # https://learn.microsoft.com/windows/win32/api/winuser/ns-winuser-msllhookstruct
        button = {1: "x1", 2: "x2"}.get(data.mouseData >> 16)
    return button


def _current_menu_keys(modifiers):
    api = _user32()
    return {vk for name in ("win", "alt") if name in modifiers
            for vk in _MODIFIER_KEYS[name] if api.GetAsyncKeyState(vk) & 0x8000}


def _mask_modifier_menu():
    # 0xE8 is unassigned: a paired press/release prevents a bare Alt/Win menu
    # after the mouse gesture without swallowing the real modifier release.
    # https://learn.microsoft.com/windows/win32/inputdev/virtual-key-codes
    inputs = (_Input * 2)(
        _Input(1, _InputUnion(ki=_KeyboardInput(wVk=0xE8))),
        _Input(1, _InputUnion(ki=_KeyboardInput(wVk=0xE8, dwFlags=2))),
    )
    if _user32().SendInput(2, inputs, ctypes.sizeof(_Input)) != 2:
        raise OSError("Could not mask the modifier menu for quick capture")


class _Win32Hooks(threading.Thread):
    """Both low-level hooks share one message loop and deterministic ordering."""

    def __init__(self, mouse, keyboard, failure):
        super().__init__(name="QuickCaptureInput", daemon=True)
        self._mouse = mouse
        self._keyboard = keyboard
        self._failure = failure
        self._stopping = threading.Event()
        self._thread_id = None

    def stop(self):
        self._stopping.set()
        thread_id = self._thread_id
        if thread_id is not None:
            api = ctypes.WinDLL("user32", use_last_error=True)
            api.PostThreadMessageW.argtypes = [wintypes.DWORD, wintypes.UINT,
                                               ctypes.c_size_t, ctypes.c_ssize_t]
            api.PostThreadMessageW.restype = wintypes.BOOL
            api.PostThreadMessageW(thread_id, 0x0012, 0, 0)  # WM_QUIT

    def run(self):
        hooks = []
        try:
            api = ctypes.WinDLL("user32", use_last_error=True)
            kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            proc = ctypes.WINFUNCTYPE(ctypes.c_ssize_t, ctypes.c_int,
                                     ctypes.c_size_t, ctypes.c_ssize_t)
            api.SetWindowsHookExW.argtypes = [ctypes.c_int, proc, wintypes.HINSTANCE, wintypes.DWORD]
            api.SetWindowsHookExW.restype = ctypes.c_void_p
            api.CallNextHookEx.argtypes = [ctypes.c_void_p, ctypes.c_int,
                                           ctypes.c_size_t, ctypes.c_ssize_t]
            api.CallNextHookEx.restype = ctypes.c_ssize_t
            api.UnhookWindowsHookEx.argtypes = [ctypes.c_void_p]
            api.UnhookWindowsHookEx.restype = wintypes.BOOL
            api.PeekMessageW.argtypes = [ctypes.POINTER(wintypes.MSG), wintypes.HWND,
                                        wintypes.UINT, wintypes.UINT, wintypes.UINT]
            api.PeekMessageW.restype = wintypes.BOOL
            api.GetMessageW.argtypes = [ctypes.POINTER(wintypes.MSG), wintypes.HWND,
                                       wintypes.UINT, wintypes.UINT]
            api.GetMessageW.restype = wintypes.BOOL
            kernel.GetCurrentThreadId.argtypes = []
            kernel.GetCurrentThreadId.restype = wintypes.DWORD

            def callback(handler, data_type):
                def dispatch(code, message, address):
                    if code >= 0 and not self._stopping.is_set():
                        try:
                            data = ctypes.cast(address, ctypes.POINTER(data_type)).contents
                            if handler(message, data):
                                return 1
                        except Exception as exc:
                            self._failure(str(exc))
                            self.stop()
                    return api.CallNextHookEx(None, code, message, address)
                return proc(dispatch)

            # Keep the ctypes callbacks strongly referenced until after unhook.
            callbacks = (callback(self._mouse, _MouseData), callback(self._keyboard, _KeyboardData))
            message = wintypes.MSG()
            api.PeekMessageW(ctypes.byref(message), None, 0, 0, 0)
            self._thread_id = kernel.GetCurrentThreadId()
            if self._stopping.is_set():
                return
            for hook_id, hook_callback in zip((14, 13), callbacks):
                hook = api.SetWindowsHookExW(hook_id, hook_callback, None, 0)
                if not hook:
                    raise ctypes.WinError(ctypes.get_last_error())
                hooks.append(hook)
            while not self._stopping.is_set():
                result = api.GetMessageW(ctypes.byref(message), None, 0, 0)
                if result == -1:
                    raise ctypes.WinError(ctypes.get_last_error())
                if result == 0:
                    break
        except Exception as exc:
            if not self._stopping.is_set():
                self._failure(str(exc))
        finally:
            for hook in reversed(hooks):
                api.UnhookWindowsHookEx(hook)
            self._thread_id = None


class QuickCaptureInput(QObject):
    """Stateful global drag input; connect public signals with QueuedConnection."""

    event = Signal(str, int, int, int)  # kind, gesture_id, physical screen x/y
    moved = Signal(int)  # gesture_id; at most one pending notification per gesture
    failure = Signal(str)
    _drained = Signal()

    def __init__(self, parent=None):
        super().__init__(parent)
        self._lock = threading.RLock()
        self._bindings = frozenset()
        # Modifiers and button of the current gesture; key handling during a drag uses these.
        self._modifiers = frozenset()
        self._button = None
        self._enabled = False
        self._blocked = False
        self._closed = False
        self._backend = None
        self._generation = 0
        self._gesture_id = 0
        self._invalid_through = 0
        self._position = (0, 0)
        self._pending_move = None
        self._dragging = False
        # Buttons whose press was swallowed; their release must be swallowed too.
        self._claimed_buttons = set()
        self._claimed_escape = False
        self._pending_menu_keys = set()
        self._drained.connect(self._sync_backend, Qt.ConnectionType.QueuedConnection)

    def take_position(self, gesture_id: int):
        """Consume the latest move, leaving a newer gesture's notification intact."""
        with self._lock:
            if self._pending_move is None or self._pending_move != gesture_id:
                return None
            self._pending_move = None
            return self._position

    @property
    def dragging(self):
        with self._lock:
            return self._dragging

    def accepts(self, gesture_id: int) -> bool:
        """Validate queued start/finish events; always deliver matching cancels.

        Reconfiguration and cancellation invalidate even a completed gesture
        whose finish event has not yet reached the GUI thread.
        """
        with self._lock:
            return not self._closed and self._invalid_through < gesture_id <= self._gesture_id

    def gesture_binding(self, gesture_id: int):
        """(modifiers, button) of the current gesture only; a newer gesture may start before a queued start arrives."""
        with self._lock:
            return (self._modifiers, self._button) if gesture_id == self._gesture_id else None

    def configure(self, bindings, enabled: bool):
        """Each binding is (modifier set, button); the held modifiers must equal the set exactly."""
        bindings = frozenset((frozenset(modifiers), button) for modifiers, button in bindings)
        valid = bool(bindings) and all(
            modifiers and len(modifiers) <= 2 and modifiers <= _MODIFIER_KEYS.keys() and button in _BUTTONS
            for modifiers, button in bindings
        )
        requested_enabled = bool(enabled)
        with self._lock:
            if self._closed:
                return
            enabled = bool(enabled and valid)
            if bindings != self._bindings or not enabled:
                self._cancel_locked()
            self._bindings, self._enabled = bindings, enabled
        if not valid and requested_enabled:
            self.failure.emit("Quick capture requires one or two modifiers")
        self._sync_backend()

    def cancel(self):
        with self._lock:
            self._cancel_locked()

    def set_blocked(self, blocked: bool):
        """GUI-owned availability; hooks never inspect windows or modal state."""
        with self._lock:
            self._blocked = bool(blocked)
            if self._blocked:
                self._cancel_locked()

    def _cancel_locked(self):
        self._invalid_through = self._gesture_id
        self._pending_move = None
        if self._dragging:
            self._dragging = False
            self.event.emit("cancel", self._gesture_id, *self._position)

    def close(self):
        with self._lock:
            self._closed = True
            self._enabled = False
            self._cancel_locked()
            self._claimed_buttons.clear()
            self._claimed_escape = False
            self._pending_menu_keys.clear()
        self._sync_backend()

    def _sync_backend(self):
        with self._lock:
            needed = not self._closed and (self._enabled or self._claimed_buttons or self._claimed_escape
                                          or self._pending_menu_keys)
            if needed and self._backend is None:
                self._generation += 1
                generation = self._generation
                backend = _Win32Hooks(
                    lambda msg, data: self._mouse_event(generation, msg, data),
                    lambda msg, data: self._keyboard_event(generation, msg, data),
                    lambda error: self._backend_failed(generation, error),
                )
                self._backend = backend
                try:
                    backend.start()
                except Exception as exc:
                    self._backend_failed(generation, str(exc))
            elif not needed and self._backend is not None:
                backend, self._backend = self._backend, None
                self._generation += 1
                backend.stop()

    def _backend_failed(self, generation, error):
        with self._lock:
            if generation != self._generation or self._closed:
                return
            self._enabled = False
            self._cancel_locked()
            self._claimed_buttons.clear()
            self._claimed_escape = False
            self._pending_menu_keys.clear()
            backend, self._backend = self._backend, None
            self._generation += 1
            if backend is not None:
                backend.stop()
            self.failure.emit(error)

    def _mouse_event(self, generation, message, data):
        if data.flags & 0x03:  # LLMHF_INJECTED / LOWER_IL_INJECTED
            return False
        with self._lock:
            if generation != self._generation or self._closed:
                return False
            self._position = (int(data.pt.x), int(data.pt.y))
            button = _button_of(_BUTTON_DOWN, message, data)
            if button is not None:
                return self._button_down(button)
            button = _button_of(_BUTTON_UP, message, data)
            if button is not None:
                return self._button_up(button)
            if message == WM_MOUSEMOVE and self._dragging and self._pending_move is None:
                # High-rate input only replaces the latest point until the GUI
                # consumes it; never queue a repaint for every native packet.
                self._pending_move = self._gesture_id
                self.moved.emit(self._gesture_id)
            if message in (WM_MOUSEWHEEL, WM_MOUSEHWHEEL) and self._dragging:
                # The desktop stays live: scrolling would move the content being framed.
                return True
            # Keep native cursor motion alive. The paired button events are
            # suppressed, so the underlying application cannot start a drag.
            return False

    def _button_down(self, button):
        if button in self._claimed_buttons:
            return True
        if self._dragging:
            # Same reason as the wheel: a click would change the content being framed.
            self._claimed_buttons.add(button)
            return True
        held = _current_modifiers() if self._enabled and not self._blocked else None
        if (held, button) not in self._bindings:
            return False
        self._modifiers, self._button = held, button
        self._gesture_id += 1
        self._pending_move = None
        self._dragging = True
        self._claimed_buttons.add(button)
        # Win auto-repeat can re-arm Start after an early mask. Keep
        # the actual held keys until their release, even after capture,
        # cancellation or a settings change has ended this gesture.
        self._pending_menu_keys.update(_current_menu_keys(self._modifiers))
        self.event.emit("start", self._gesture_id, *self._position)
        return True

    def _button_up(self, button):
        if button not in self._claimed_buttons:
            return False
        self._claimed_buttons.discard(button)
        if button == self._button:
            self._pending_move = None
            if self._dragging:
                self._dragging = False
                self.event.emit("finish", self._gesture_id, *self._position)
        self._drained.emit()
        return True

    def _keyboard_event(self, generation, message, data):
        if data.flags & 0x12:  # LLKHF_INJECTED / LOWER_IL_INJECTED
            return False
        with self._lock:
            if generation != self._generation or self._closed:
                return False
            pressed = message in (WM_KEYDOWN, WM_SYSKEYDOWN)
            released = message in (WM_KEYUP, WM_SYSKEYUP)
            modifier = _KEY_MODIFIERS.get(data.vkCode)
            if pressed and self._dragging and modifier in self._modifiers and modifier in {"win", "alt"}:
                self._pending_menu_keys.add(data.vkCode)
            if released and data.vkCode in self._pending_menu_keys:
                self._pending_menu_keys.remove(data.vkCode)
                try:
                    # Run on the hook thread immediately before forwarding UP,
                    # after any repeat events or capture-induced focus change.
                    _mask_modifier_menu()
                except Exception as exc:
                    self.failure.emit(str(exc))
                self._drained.emit()
                # Never swallow the real release: Windows saw the original
                # keydown and must also see keyup to clear its modifier state.
            if data.vkCode == VK_ESCAPE:
                if pressed and (self._dragging or self._claimed_escape):
                    self._claimed_escape = True
                    self._cancel_locked()
                    return True
                if released and self._claimed_escape:
                    self._claimed_escape = False
                    self._drained.emit()
                    return True
            if self._dragging and released and modifier in self._modifiers:
                # LowLevelKeyboardProc runs before this key's async state updates.
                if modifier not in _current_modifiers(exclude_vk=data.vkCode):
                    self._cancel_locked()
            return False
