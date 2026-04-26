"""
统一快捷键管理器

合并了两套机制：
  1. 系统级全局热键 (Windows RegisterHotKey / WM_HOTKEY)
  2. 应用内 Qt KeyPress 事件分发

两者共用同一条优先级 handler 链，解决模块间快捷键冲突问题。

架构：
    ShortcutManager (单例，安装在 QApplication 上)
      ├── 注册/注销 Windows 全局热键
      ├── _HotkeyEventFilter — 拦截 WM_HOTKEY，交由 handler 链决定是否执行回调
      ├── eventFilter         — 拦截 Qt KeyPress，按优先级分发
      └── handler 列表        — 统一的优先级分发链

每个模块实现 ShortcutHandler 接口：
    - is_active() → bool          : 当前是否应该接收按键
    - handle_key(event) → bool    : 处理 Qt KeyPress，返回 True 表示已消费
    - handle_hotkey(hotkey_id, callback) → bool  (可选覆写)
        返回 True = 拦截此次系统热键，不执行原回调

优先级（数字越大越优先）：
    200  热键录入框    — 需要捕获系统热键本身
    100  截图模式      — 全屏遮罩
     80  GIF 绘制模式  — 绘制层活跃时
     60  剪贴板窗口    — 弹出时
     50  钉图编辑模式  — 画布工具激活时
     40  钉图普通模式  — 鼠标在钉图上方时
"""

from __future__ import annotations

import ctypes
from abc import ABC, abstractmethod
from ctypes import wintypes
from typing import Callable, Dict, List, Optional, Set, Tuple

from PySide6.QtCore import QAbstractNativeEventFilter, QEvent, QObject, Qt
from PySide6.QtWidgets import (
    QApplication, QComboBox, QDoubleSpinBox, QGraphicsView,
    QLineEdit, QPlainTextEdit, QSpinBox, QTextEdit,
)

from core import log_debug, log_info, log_warning, log_error, safe_event
from core.logger import log_exception

# ======================================================================
# Windows API 常量
# ======================================================================
WM_HOTKEY = 0x0312
MOD_ALT = 0x0001
MOD_CONTROL = 0x0002
MOD_SHIFT = 0x0004
MOD_WIN = 0x0008
MOD_NOREPEAT = 0x4000


# ======================================================================
# Handler 接口
# ======================================================================

class ShortcutHandler(ABC):
    """快捷键处理器接口，各模块实现此接口注册到管理器"""

    @abstractmethod
    def is_active(self) -> bool:
        """当前是否处于活跃状态（应该接收按键）"""
        ...

    @abstractmethod
    def handle_key(self, event) -> bool:
        """
        处理 Qt KeyPress 事件。

        Args:
            event: QKeyEvent

        Returns:
            True  — 已消费此事件
            False — 不处理，交给下一个 handler
        """
        ...

    def handle_hotkey(self, hotkey_id: int, callback: Callable) -> bool:
        """
        处理系统级 WM_HOTKEY 事件（可选覆写）。

        当 Windows 全局热键触发时，在执行原始回调之前，
        ShortcutManager 会先按优先级询问每个活跃 handler。
        返回 True 表示拦截此次热键（不执行原回调）。

        默认实现：不拦截，返回 False。
        """
        return False

    @property
    @abstractmethod
    def priority(self) -> int:
        """优先级，数字越大越优先"""
        ...

    @property
    def handler_name(self) -> str:
        """用于日志的名称"""
        return self.__class__.__name__


# ======================================================================
# Windows 原生事件过滤器
# ======================================================================

class _HotkeyEventFilter(QAbstractNativeEventFilter):
    """拦截 Windows WM_HOTKEY 消息，委托给 ShortcutManager 分发。"""

    def __init__(self, manager: 'ShortcutManager',
                 id_to_callback: Dict[int, Callable]):
        super().__init__()
        self._manager = manager
        self._id_to_callback = id_to_callback

    def nativeEventFilter(self, eventType, message):
        try:
            if eventType in (b"windows_generic_MSG", b"windows_dispatcher_MSG"):
                msg = wintypes.MSG.from_address(int(message))
                if msg.message == WM_HOTKEY:
                    hotkey_id = msg.wParam
                    cb = self._id_to_callback.get(hotkey_id)
                    if cb:
                        # 先过 handler 链，看有没有人要拦截
                        if self._manager._dispatch_hotkey(hotkey_id, cb):
                            return True, 0
                        # 没人拦截，执行原始回调
                        try:
                            cb()
                        except Exception as e:
                            log_exception(e, f"热键回调 id={hotkey_id}")
                        return True, 0
        except (KeyboardInterrupt, SystemExit):
            raise
        except Exception as e:
            log_exception(e, "nativeEventFilter")
        return False, 0


# ======================================================================
# 统一管理器（单例）
# ======================================================================

class ShortcutManager(QObject):
    """
    统一快捷键管理器（单例）

    同时管理：
      - Windows RegisterHotKey 全局热键
      - Qt 应用内 KeyPress 事件
    """

    _instance: Optional['ShortcutManager'] = None

    # 类级变量，跟踪当前进程所有已注册的热键 (mods, vk)
    _registered_keys_global: Set[Tuple[int, int]] = set()

    def __init__(self):
        super().__init__()
        # ── handler 链 ──
        self._handlers: List[ShortcutHandler] = []

        # ── 系统热键 ──
        self._id_to_callback: Dict[int, Callable] = {}
        self._id_to_metadata: Dict[int, Tuple[int, int]] = {}  # id → (mods, vk)
        self._next_hotkey_id = 1

        # 安装原生事件过滤器（WM_HOTKEY）
        self._native_filter = _HotkeyEventFilter(self, self._id_to_callback)

    @classmethod
    def instance(cls) -> 'ShortcutManager':
        if cls._instance is None:
            cls._instance = cls()
            app = QApplication.instance()
            if app:
                app.installEventFilter(cls._instance)
                app.installNativeEventFilter(cls._instance._native_filter)
                log_info("ShortcutManager 已安装（KeyPress + WM_HOTKEY）", "Shortcut")
        return cls._instance

    # ==================================================================
    # Handler 注册 / 注销
    # ==================================================================

    def register(self, handler: ShortcutHandler):
        """注册一个快捷键处理器"""
        if handler not in self._handlers:
            self._handlers.append(handler)
            self._handlers.sort(key=lambda h: h.priority, reverse=True)
            log_debug(
                f"注册 handler: {handler.handler_name} (优先级 {handler.priority})，"
                f"当前共 {len(self._handlers)} 个",
                "Shortcut",
            )

    def unregister(self, handler: ShortcutHandler):
        """注销一个快捷键处理器"""
        try:
            self._handlers.remove(handler)
            log_debug(f"注销 handler: {handler.handler_name}", "Shortcut")
        except ValueError:
            pass

    # ==================================================================
    # Qt KeyPress 分发
    # ==================================================================

    # 这些键属于结构性 / 功能键，即使焦点在文字框里也应交给快捷键系统
    _PASSTHROUGH_KEYS = frozenset({
        Qt.Key.Key_Escape, Qt.Key.Key_Tab, Qt.Key.Key_Backtab,
        Qt.Key.Key_F1, Qt.Key.Key_F2, Qt.Key.Key_F3, Qt.Key.Key_F4,
        Qt.Key.Key_F5, Qt.Key.Key_F6, Qt.Key.Key_F7, Qt.Key.Key_F8,
        Qt.Key.Key_F9, Qt.Key.Key_F10, Qt.Key.Key_F11, Qt.Key.Key_F12,
    })

    def _is_text_input_active(self, event) -> bool:
        """焦点在文字输入控件上，且按键属于文字输入类（非结构键）"""
        if event.key() in self._PASSTHROUGH_KEYS:
            return False

        focus = QApplication.focusWidget()
        if focus is None:
            return False

        # 常见文字输入控件
        if isinstance(focus, (QLineEdit, QTextEdit, QPlainTextEdit,
                              QSpinBox, QDoubleSpinBox)):
            return True
        if isinstance(focus, QComboBox) and focus.isEditable():
            return True

        # QGraphicsView 中正在编辑 TextItem
        if isinstance(focus, QGraphicsView):
            scene = focus.scene()
            if scene:
                from PySide6.QtWidgets import QGraphicsTextItem
                fi = scene.focusItem()
                if (isinstance(fi, QGraphicsTextItem)
                        and fi.hasFocus()
                        and bool(fi.textInteractionFlags()
                                 & Qt.TextInteractionFlag.TextEditorInteraction)):
                    return True

        return False

    @safe_event
    def eventFilter(self, obj, event):
        if event.type() != QEvent.Type.KeyPress:
            return False

        # 文字输入控件获焦时，优先让控件处理按键
        if self._is_text_input_active(event):
            return False

        for handler in self._handlers:
            try:
                if handler.is_active():
                    if handler.handle_key(event):
                        log_debug(
                            f"按键被 {handler.handler_name} 消费 "
                            f"(key=0x{event.key():X})",
                            "Shortcut",
                        )
                        return True
            except RuntimeError:
                continue
            except Exception as e:
                log_exception(e, f"ShortcutManager: {handler.handler_name}.handle_key")
                continue

        return False

    # ==================================================================
    # WM_HOTKEY 分发（由 _HotkeyEventFilter 调用）
    # ==================================================================

    def _dispatch_hotkey(self, hotkey_id: int, callback: Callable) -> bool:
        """
        按优先级询问 handler 链是否要拦截此次系统热键。

        Returns:
            True  — 某个 handler 已拦截（不执行原回调）
            False — 没人拦截，应执行原回调
        """
        for handler in self._handlers:
            try:
                if handler.is_active():
                    if handler.handle_hotkey(hotkey_id, callback):
                        log_debug(
                            f"系统热键被 {handler.handler_name} 拦截 "
                            f"(id={hotkey_id})",
                            "Shortcut",
                        )
                        return True
            except RuntimeError:
                continue
            except Exception as e:
                log_exception(e, f"ShortcutManager: {handler.handler_name}.handle_hotkey")
                continue
        return False

    # ==================================================================
    # 系统全局热键注册 / 注销
    # ==================================================================

    def register_hotkey(self, hotkey_str: str, callback: Callable) -> bool:
        """注册一个 Windows 全局热键"""
        try:
            mods, vk = self._parse_hotkey(hotkey_str)
            hid = self._next_hotkey_id

            if ctypes.windll.user32.RegisterHotKey(None, hid, mods, vk):
                self._id_to_callback[hid] = callback
                self._id_to_metadata[hid] = (mods, vk)
                ShortcutManager._registered_keys_global.add((mods, vk))
                self._next_hotkey_id += 1
                return True
            else:
                log_warning(f"Failed to register hotkey: {hotkey_str}", module="热键")
                return False
        except Exception as e:
            log_error(f"Error registering hotkey {hotkey_str}: {e}", module="热键")
            return False

    def check_hotkey_availability(self, hotkey_str: str) -> bool:
        """检查快捷键是否可用（通过临时注册测试）"""
        try:
            mods, vk = self._parse_hotkey(hotkey_str)

            if (mods, vk) in ShortcutManager._registered_keys_global:
                return True

            test_id = 9999
            success = ctypes.windll.user32.RegisterHotKey(None, test_id, mods, vk)
            if success:
                ctypes.windll.user32.UnregisterHotKey(None, test_id)
                return True
            return False
        except Exception as e:
            log_exception(e, "检查快捷键可用性")
            return False

    def unregister_all_hotkeys(self):
        """注销所有 Windows 全局热键"""
        for hid in list(self._id_to_callback.keys()):
            ctypes.windll.user32.UnregisterHotKey(None, hid)
            meta = self._id_to_metadata.get(hid)
            if meta and meta in ShortcutManager._registered_keys_global:
                ShortcutManager._registered_keys_global.discard(meta)

        self._id_to_callback.clear()
        self._id_to_metadata.clear()

    # ==================================================================
    # 热键字符串解析
    # ==================================================================

    @staticmethod
    def _parse_hotkey(hotkey: str) -> Tuple[int, int]:
        """将 'ctrl+shift+a' 风格字符串解析为 (modifiers, vk)。"""
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
            elif p == "shift":
                mods |= MOD_SHIFT
            elif p in ("win", "meta", "super"):
                mods |= MOD_WIN
            else:
                key = p

        if not key:
            raise ValueError("缺少主键位")

        vk = None
        if len(key) == 1 and 'a' <= key <= 'z':
            vk = ord(key.upper())
        elif key.isdigit() and len(key) == 1:
            vk = ord(key)
        elif key.startswith('f') and key[1:].isdigit():
            n = int(key[1:])
            if 1 <= n <= 24:
                vk = 0x70 + (n - 1)
        elif key in ("printscreen", "prtsc"):
            vk = 0x2C
        elif key == "esc":
            vk = 0x1B
        elif key in ("`", "oem3", "backquote", "grave"):
            vk = 0xC0
        elif key in ("-", "minus"):
            vk = 0xBD
        elif key in ("=", "equals", "equal"):
            vk = 0xBB
        elif key in ("[", "lbracket"):
            vk = 0xDB
        elif key in ("]", "rbracket"):
            vk = 0xDD
        elif key in ("\\", "backslash"):
            vk = 0xDC
        elif key in (";", "semicolon"):
            vk = 0xBA
        elif key in ("'", "quote"):
            vk = 0xDE
        elif key in (",", "comma"):
            vk = 0xBC
        elif key in (".", "period"):
            vk = 0xBE
        elif key in ("/", "slash"):
            vk = 0xBF

        if vk is None:
            raise ValueError(f"不支持的键: {key}")

        mods |= MOD_NOREPEAT
        return mods, vk


# ======================================================================
# HotkeySystem — 对外公开的热键注册入口（委托给 ShortcutManager 单例）
# ======================================================================

class HotkeySystem:
    """对外公开的热键注册入口，委托给 ShortcutManager 单例。"""

    def __init__(self):
        self._mgr = ShortcutManager.instance()

    def register_hotkey(self, hotkey_str: str, callback: Callable) -> bool:
        return self._mgr.register_hotkey(hotkey_str, callback)

    def check_hotkey_availability(self, hotkey_str: str) -> bool:
        return self._mgr.check_hotkey_availability(hotkey_str)

    def unregister_all(self):
        self._mgr.unregister_all_hotkeys()


# ======================================================================
# 应用内快捷键工具
# ======================================================================

# ── 权威键名映射表（双向）──────────────────────────────
# 字符串名 → Qt.Key  和  Qt.Key → 显示名  共享同一份数据源。
# hotkey_edit.py / inapp_key_edit.py / parse_shortcut_to_qt 均从此处导入。

def _build_key_tables():
    """延迟构建（避免模块级导入 Qt）。首次访问后缓存在模块级变量中。"""
    from PySide6.QtCore import Qt as _Qt

    # (显示名, Qt.Key, *别名)  — 别名用于从配置字符串解析
    _RAW = [
        ("Esc",       _Qt.Key.Key_Escape,    "escape"),
        ("Tab",       _Qt.Key.Key_Tab),
        ("Backtab",   _Qt.Key.Key_Backtab),
        ("Backspace", _Qt.Key.Key_Backspace),
        ("Enter",     _Qt.Key.Key_Return,    "return"),
        ("Enter",     _Qt.Key.Key_Enter),
        ("Insert",    _Qt.Key.Key_Insert),
        ("Delete",    _Qt.Key.Key_Delete,    "del"),
        ("Pause",     _Qt.Key.Key_Pause),
        ("Print",     _Qt.Key.Key_Print,     "printscreen", "prtsc"),
        ("SysReq",    _Qt.Key.Key_SysReq),
        ("Clear",     _Qt.Key.Key_Clear),
        ("Home",      _Qt.Key.Key_Home),
        ("End",       _Qt.Key.Key_End),
        ("Left",      _Qt.Key.Key_Left),
        ("Up",        _Qt.Key.Key_Up),
        ("Right",     _Qt.Key.Key_Right),
        ("Down",      _Qt.Key.Key_Down),
        ("PageUp",    _Qt.Key.Key_PageUp),
        ("PageDown",  _Qt.Key.Key_PageDown),
        ("Space",     _Qt.Key.Key_Space),
    ]

    # Qt.Key → 显示名（UI 录入框用）
    qt_key_to_display: Dict[int, str] = {}
    # 小写字符串 → Qt.Key（配置解析用）
    str_to_qt_key: Dict[str, int] = {}

    for entry in _RAW:
        display_name, qt_key = entry[0], entry[1]
        aliases = entry[2:] if len(entry) > 2 else ()

        qt_key_to_display[qt_key] = display_name
        # 用显示名的小写作为主键
        str_to_qt_key[display_name.lower()] = qt_key
        for alias in aliases:
            str_to_qt_key[alias.lower()] = qt_key

    return qt_key_to_display, str_to_qt_key


# 模块级缓存，首次访问时构建
_QT_KEY_TO_DISPLAY: Optional[Dict[int, str]] = None
_STR_TO_QT_KEY: Optional[Dict[str, int]] = None


def get_key_display_map() -> Dict[int, str]:
    """返回 {Qt.Key → 显示名} 字典（UI 录入框使用）"""
    global _QT_KEY_TO_DISPLAY, _STR_TO_QT_KEY
    if _QT_KEY_TO_DISPLAY is None:
        _QT_KEY_TO_DISPLAY, _STR_TO_QT_KEY = _build_key_tables()
    return _QT_KEY_TO_DISPLAY


def get_key_parse_map() -> Dict[str, int]:
    """返回 {小写字符串 → Qt.Key} 字典（配置解析使用）"""
    global _QT_KEY_TO_DISPLAY, _STR_TO_QT_KEY
    if _STR_TO_QT_KEY is None:
        _QT_KEY_TO_DISPLAY, _STR_TO_QT_KEY = _build_key_tables()
    return _STR_TO_QT_KEY

def parse_shortcut_to_qt(text: str):
    """
    将 "ctrl+c" / "pageup" / "shift+c" 风格字符串解析为 (Qt.Key, Qt.KeyboardModifier)。

    返回 None 表示解析失败。供各 ShortcutHandler 在 __init__ 中一次性调用。
    """
    from PySide6.QtCore import Qt as _Qt

    if not text or not isinstance(text, str):
        return None

    parts = [p.strip() for p in text.lower().split("+") if p.strip()]
    if not parts:
        return None

    _MOD_MAP = {
        "ctrl": _Qt.KeyboardModifier.ControlModifier,
        "shift": _Qt.KeyboardModifier.ShiftModifier,
        "alt": _Qt.KeyboardModifier.AltModifier,
    }
    key_map = get_key_parse_map()

    mods = _Qt.KeyboardModifier.NoModifier
    key = _Qt.Key.Key_unknown

    for p in parts:
        if p in _MOD_MAP:
            mods |= _MOD_MAP[p]
        elif p in key_map:
            key = _Qt.Key(key_map[p])
        elif len(p) == 1 and p.isalpha():
            key = _Qt.Key(ord(p.upper()))
        elif p.startswith("f") and p[1:].isdigit():
            fn = int(p[1:])
            if 1 <= fn <= 24:
                key = _Qt.Key(_Qt.Key.Key_F1.value + fn - 1)

    if key == _Qt.Key.Key_unknown:
        return None
    return (key, mods)


def load_inapp_bindings(keys_of_interest: Optional[List[str]] = None) -> Dict:
    """
    从 config_manager 读取应用内快捷键，返回 {cfg_key: (Qt.Key, Qt.KeyboardModifier)} 字典。

    Args:
        keys_of_interest: 需要读取的配置键列表，为 None 时使用全部默认键。
    """
    from settings import get_tool_settings_manager
    cfg = get_tool_settings_manager()
    defaults = cfg.APP_DEFAULT_SETTINGS

    if keys_of_interest is None:
        keys_of_interest = [
            "inapp_confirm", "inapp_pin", "inapp_undo", "inapp_redo",
            "inapp_delete",
            "inapp_copy_pin", "inapp_thumbnail", "inapp_toggle_toolbar",
            "inapp_zoom_in", "inapp_zoom_out", "inapp_translate",
        ]

    result = {}
    for k in keys_of_interest:
        text = cfg.get_inapp_shortcut(k) or defaults.get(k, "")
        parsed = parse_shortcut_to_qt(text) if text else None
        if parsed:
            result[k] = parsed
    return result


def load_move_keys() -> Dict:
    """根据配置构建鼠标微移键表 {Qt.Key: (dx, dy)}"""
    from PySide6.QtCore import Qt as _Qt
    from settings import get_tool_settings_manager
    mode = get_tool_settings_manager().get_inapp_cursor_move_mode()
    move = {}
    if mode in ("both", "wasd"):
        move.update({
            _Qt.Key.Key_W: (0, -1), _Qt.Key.Key_S: (0, 1),
            _Qt.Key.Key_A: (-1, 0), _Qt.Key.Key_D: (1, 0),
        })
    if mode in ("both", "arrows"):
        move.update({
            _Qt.Key.Key_Up: (0, -1), _Qt.Key.Key_Down: (0, 1),
            _Qt.Key.Key_Left: (-1, 0), _Qt.Key.Key_Right: (1, 0),
        })
    return move
 