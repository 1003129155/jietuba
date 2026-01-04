import ctypes
from ctypes import wintypes
from PyQt6.QtCore import QAbstractNativeEventFilter, QObject, pyqtSignal
from PyQt6.QtWidgets import QApplication
from core import log_debug, log_info, log_warning, log_error
from core.logger import log_exception

# Windows API Constants
WM_HOTKEY = 0x0312
MOD_ALT = 0x0001
MOD_CONTROL = 0x0002
MOD_SHIFT = 0x0004
MOD_WIN = 0x0008
MOD_NOREPEAT = 0x4000

class HotkeyEventFilter(QAbstractNativeEventFilter):
    """拦截 Windows 消息，处理 WM_HOTKEY。"""

    def __init__(self, id_to_callback: dict):
        super().__init__()
        self._id_to_callback = id_to_callback

    def nativeEventFilter(self, eventType, message):
        try:
            # PyQt6: message is an int (pointer)
            if eventType == b"windows_generic_MSG" or eventType == b"windows_dispatcher_MSG":
                msg = wintypes.MSG.from_address(int(message))
                if msg.message == WM_HOTKEY:
                    hotkey_id = msg.wParam
                    cb = self._id_to_callback.get(hotkey_id)
                    if cb:
                        try:
                            cb()
                        except Exception as e:
                            log_exception(e, f"热键回调 id={hotkey_id}")
                        return True, 0
        except Exception as e:
            log_exception(e, "nativeEventFilter")
        return False, 0

class HotkeySystem(QObject):
    def __init__(self):
        super().__init__()
        self._id_to_callback = {}
        self._current_id = 1
        self._filter = HotkeyEventFilter(self._id_to_callback)
        
        # Install the event filter
        app = QApplication.instance()
        if app:
            app.installNativeEventFilter(self._filter)

    def register_hotkey(self, hotkey_str: str, callback):
        """注册全局快捷键"""
        try:
            mods, vk = self._parse_hotkey(hotkey_str)
            hotkey_id = self._current_id
            
            # RegisterHotKey(hWnd, id, fsModifiers, vk)
            # hWnd=None means the hotkey is associated with the thread, not a specific window
            if ctypes.windll.user32.RegisterHotKey(None, hotkey_id, mods, vk):
                self._id_to_callback[hotkey_id] = callback
                self._current_id += 1
                return True
            else:
                log_warning(f"Failed to register hotkey: {hotkey_str}", module="热键")
                return False
        except Exception as e:
            log_error(f"Error registering hotkey {hotkey_str}: {e}", module="热键")
            return False

    def unregister_all(self):
        """注销所有快捷键"""
        for hotkey_id in self._id_to_callback.keys():
            ctypes.windll.user32.UnregisterHotKey(None, hotkey_id)
        self._id_to_callback.clear()

    def _parse_hotkey(self, hotkey: str):
        """将字符串热键解析为 (modifiers, vk)。"""
        if not hotkey or not isinstance(hotkey, str):
            raise ValueError("无效的热键字符串")

        parts = [p.strip().lower() for p in hotkey.split('+') if p.strip()]
        if not parts:
            raise ValueError("热键不能为空")

        mods = 0
        key = None

        for p in parts:
            if p in ("ctrl", "control"):  
                mods |= MOD_CONTROL
            elif p == "alt":
                mods |= MOD_ALT
            elif p in ("shift",):
                mods |= MOD_SHIFT
            elif p in ("win", "meta", "super"):
                mods |= MOD_WIN
            else:
                key = p

        if not key:
            raise ValueError("缺少主键位")

        # 映射主键到 VK
        vk = None
        # 字母
        if len(key) == 1 and 'a' <= key <= 'z':
            vk = ord(key.upper())
        # 数字 0-9
        elif key.isdigit() and len(key) == 1:
            vk = ord(key)
        # 功能键 F1-F24
        elif key.startswith('f') and key[1:].isdigit():
            n = int(key[1:])
            if 1 <= n <= 24:
                vk = 0x70 + (n - 1)  # VK_F1=0x70
        # 常见命名
        elif key in ("printscreen", "prtsc", "prtsc"):
            vk = 0x2C
        elif key == "esc":
            vk = 0x1B
        # 反引号/波浪号键 (`)
        elif key in ("`", "oem3", "backquote", "grave"):
            vk = 0xC0  # VK_OEM_3
        # 其他常见符号键
        elif key in ("-", "minus"):
            vk = 0xBD  # VK_OEM_MINUS
        elif key in ("=", "equals", "equal"):
            vk = 0xBB  # VK_OEM_PLUS
        elif key in ("[", "lbracket"):
            vk = 0xDB  # VK_OEM_4
        elif key in ("]", "rbracket"):
            vk = 0xDD  # VK_OEM_6
        elif key in ("\\", "backslash"):
            vk = 0xDC  # VK_OEM_5
        elif key in (";", "semicolon"):
            vk = 0xBA  # VK_OEM_1
        elif key in ("'", "quote"):
            vk = 0xDE  # VK_OEM_7
        elif key in (",", "comma"):
            vk = 0xBC  # VK_OEM_COMMA
        elif key in (".", "period"):
            vk = 0xBE  # VK_OEM_PERIOD
        elif key in ("/", "slash"):
            vk = 0xBF  # VK_OEM_2

        if vk is None:
            raise ValueError(f"不支持的键: {key}")

        mods |= MOD_NOREPEAT
        return mods, vk
