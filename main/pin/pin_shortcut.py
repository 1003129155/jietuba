"""
钉图快捷键控制器

通过 ShortcutManager 统一管理键盘事件，根据鼠标位置判断
是否在某个钉图窗口上方，从而实现"对着钉图按快捷键"的效果。

拆分为两个 Handler：
- PinEditShortcutHandler (priority=50): 编辑模式下处理 复制键 / ESC
- PinNormalShortcutHandler (priority=40): 非编辑模式下处理 复制键 / R / ESC
"""

from PySide6.QtCore import QObject, Qt
from PySide6.QtGui import QCursor
from core import log_info
from core.shortcut_manager import ShortcutManager, ShortcutHandler, load_inapp_bindings


class _PinHandlerBase(ShortcutHandler):
    """钉图快捷键处理器的共用基类"""

    # 需要从配置读取的钉图快捷键列表
    _PIN_KEYS = ["inapp_copy_pin", "inapp_thumbnail", "inapp_toggle_toolbar"]

    def __init__(self, controller: 'PinShortcutController'):
        self._controller = controller
        # 从配置读取钉图相关绑定
        self._bindings = load_inapp_bindings(self._PIN_KEYS)

    def reload_bindings(self):
        """重新读取配置（设置变更后调用）"""
        self._bindings = load_inapp_bindings(self._PIN_KEYS)

    def _find_pin_under_cursor(self):
        return self._controller._find_pin_under_cursor()

    def _match(self, event, cfg_key: str) -> bool:
        """检查按键事件是否匹配某个配置的快捷键"""
        binding = self._bindings.get(cfg_key)
        if not binding:
            return False
        want_key, want_mods = binding
        return event.key() == want_key and event.modifiers() == want_mods


class PinEditShortcutHandler(_PinHandlerBase):
    """钉图编辑模式快捷键（priority=50）

    当鼠标下方的钉图处于编辑模式时：
    - 可配置复制键：复制（OCR 文字优先）
    - ESC：退出编辑模式（交给 pin_canvas_view）
    """

    @property
    def priority(self) -> int:
        return 50

    @property
    def handler_name(self) -> str:
        return "PinEdit"

    def is_active(self) -> bool:
        pin = self._find_pin_under_cursor()
        if pin is None:
            return False
        return bool(pin.canvas and pin.canvas.is_editing)

    def handle_key(self, event) -> bool:
        pin = self._find_pin_under_cursor()
        if pin is None:
            return False
        if not (pin.canvas and pin.canvas.is_editing):
            return False

        key = event.key()

        # 配置的复制快捷键
        if self._match(event, "inapp_copy_pin"):
            ocr_layer = getattr(pin, 'ocr_text_layer', None)
            if ocr_layer and ocr_layer.get_selected_text():
                ocr_layer._copy_selected_text()
                return True
            pin.copy_to_clipboard()
            return True

        # 切换工具栏（编辑模式下与普通模式行为一致，hide_toolbar 会同时退出编辑）
        if self._match(event, "inapp_toggle_toolbar"):
            pin.toggle_toolbar()
            return True

        # ESC：退出编辑模式
        if key == Qt.Key.Key_Escape:
            # 如果 OCR 层有选中文字，先清除选择
            ocr_layer = getattr(pin, 'ocr_text_layer', None)
            if ocr_layer and getattr(ocr_layer, '_is_active', lambda: False)():
                if ocr_layer.selection_start or ocr_layer.selection_end:
                    ocr_layer.clear_selection()
                    return True
            pin.canvas.deactivate_tool()
            if (pin.toolbar
                    and hasattr(pin.toolbar, 'current_tool')
                    and pin.toolbar.current_tool):
                for btn in pin.toolbar.tool_buttons.values():
                    btn.setChecked(False)
                pin.toolbar.current_tool = None
            return True

        # 编辑模式下其他按键不吞掉
        return False


class PinNormalShortcutHandler(_PinHandlerBase):
    """钉图普通模式快捷键（priority=40）

    当鼠标下方的钉图处于非编辑模式时：
    - 可配置复制键：复制钉图内容
    - R：切换缩略图模式
    - ESC：关闭钉图窗口
    """

    @property
    def priority(self) -> int:
        return 40

    @property
    def handler_name(self) -> str:
        return "PinNormal"

    def is_active(self) -> bool:
        pin = self._find_pin_under_cursor()
        if pin is None:
            return False
        # 非编辑模式才激活
        if pin.canvas and pin.canvas.is_editing:
            return False
        return True

    def handle_key(self, event) -> bool:
        pin = self._find_pin_under_cursor()
        if pin is None:
            return False
        if pin.canvas and pin.canvas.is_editing:
            return False

        key = event.key()

        # 配置的复制快捷键
        if self._match(event, "inapp_copy_pin"):
            ocr_layer = getattr(pin, 'ocr_text_layer', None)
            if ocr_layer and ocr_layer.get_selected_text():
                ocr_layer._copy_selected_text()
                return True
            pin.copy_to_clipboard()
            return True

        # 切换缩略图模式
        if self._match(event, "inapp_thumbnail"):
            pin.toggle_thumbnail_mode()
            return True

        # 切换工具栏
        if self._match(event, "inapp_toggle_toolbar"):
            pin.toggle_toolbar()
            return True

        # ESC：关闭钉图
        if key == Qt.Key.Key_Escape:
            pin.close_window()
            return True

        return False


class PinShortcutController(QObject):
    """
    钉图全局快捷键控制器

    现已通过 ShortcutManager 注册两个 Handler 来处理快捷键，
    不再使用应用级 eventFilter。

    快捷键列表：
    - Ctrl+C : 复制钉图内容到剪贴板
    - R      : 切换缩略图模式
    - ESC    : 关闭钉图窗口
    """

    _instance = None

    def __init__(self):
        super().__init__()
        self._pin_windows = []  # 由 PinManager 维护

        # 创建并注册两个 Handler
        self._edit_handler = PinEditShortcutHandler(self)
        self._normal_handler = PinNormalShortcutHandler(self)
        mgr = ShortcutManager.instance()
        mgr.register(self._edit_handler)
        mgr.register(self._normal_handler)

    @classmethod
    def instance(cls):
        if cls._instance is None:
            cls._instance = cls()
        return cls._instance

    # ------------------------------------------------------------------
    # 窗口注册 / 注销
    # ------------------------------------------------------------------

    def register(self, pin_window):
        """注册一个钉图窗口"""
        if pin_window not in self._pin_windows:
            self._pin_windows.append(pin_window)

    def unregister(self, pin_window):
        """注销一个钉图窗口"""
        try:
            self._pin_windows.remove(pin_window)
        except ValueError:
            pass

    # ------------------------------------------------------------------
    # 辅助
    # ------------------------------------------------------------------

    def _find_pin_under_cursor(self):
        """查找鼠标当前位置下方的钉图窗口（最上层优先）"""
        cursor_pos = QCursor.pos()

        # 清理已关闭的窗口引用
        self._pin_windows = [
            w for w in self._pin_windows
            if self._is_alive(w)
        ]

        # 倒序遍历（后创建的在上层）
        for pin in reversed(self._pin_windows):
            try:
                if pin.geometry().contains(cursor_pos):
                    return pin
            except RuntimeError:
                continue

        return None

    @staticmethod
    def _is_alive(win) -> bool:
        """检查窗口的 C++ 对象是否还有效"""
        try:
            _ = win.isVisible()
            return not getattr(win, '_is_closed', True)
        except RuntimeError:
            return False
 