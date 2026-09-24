"""快捷键录入框组件 - 系统快捷键冲突检测

用户按下按键时实时检测是否与系统全局快捷键冲突，
提供视觉反馈，防止注册冲突的快捷键。
"""

from PySide6.QtWidgets import QWidget, QHBoxLayout
from PySide6.QtCore import Qt, QTimer, Signal
from PySide6.QtGui import QKeyEvent, QKeySequence

from core.shortcut_manager import (
    ShortcutManager, ShortcutHandler, get_key_display_map, hotkey_identity,
)
from core import safe_event
from core.i18n import make_tr
from core.ui_scale import dialog_scaled

from .key_chip import STATUS_GAP, KeyChipLineEdit, StatusIcon

_tr = make_tr("HotkeyEdit")


class _HotkeyEditHandler(ShortcutHandler):
    """
    热键录入框专用 handler（优先级 200，最高）。

    聚焦时注册，失焦时注销。
    - handle_key:    拦截 Qt KeyPress，阻止截图/钉图等 handler 抢走按键
    - handle_hotkey: 拦截 WM_HOTKEY 系统热键，阻止触发截图/剪贴板回调
    """

    def __init__(self, line_edit: '_HotkeyLineEdit'):
        self._line_edit = line_edit
        self._active = False

    @property
    def priority(self) -> int:
        return 200

    @property
    def handler_name(self) -> str:
        return "HotkeyEditCapture"

    def is_active(self) -> bool:
        return self._active

    def handle_key(self, event) -> bool:
        # 把按键事件直接转发给 _HotkeyLineEdit 处理，然后吞掉防止低优先级 handler 收到
        self._line_edit.keyPressEvent(event)
        return True

    def handle_hotkey(self, hotkey_id: int, callback) -> bool:
        # 拦截系统热键，并合成 Qt KeyPress 事件投递给输入框
        from PySide6.QtCore import QEvent
        from PySide6.QtGui import QKeyEvent
        from core.shortcut_manager import (
            ShortcutManager,
            MOD_CONTROL, MOD_ALT, MOD_SHIFT, MOD_WIN,
        )
        mgr = ShortcutManager.instance()
        meta = mgr._id_to_metadata.get(hotkey_id)
        if meta is None:
            return True  # 不知道是什么键，直接拦截不触发

        mods_bits, vk = meta

        # 把 Windows modifier bits 转成 Qt modifier flags
        qt_mods = Qt.KeyboardModifier.NoModifier
        if mods_bits & MOD_CONTROL:
            qt_mods |= Qt.KeyboardModifier.ControlModifier
        if mods_bits & MOD_ALT:
            qt_mods |= Qt.KeyboardModifier.AltModifier
        if mods_bits & MOD_SHIFT:
            qt_mods |= Qt.KeyboardModifier.ShiftModifier
        if mods_bits & MOD_WIN:
            qt_mods |= Qt.KeyboardModifier.MetaModifier

        # vk → Qt.Key（字母 A-Z / 数字 0-9 / F1-F24）
        qt_key = Qt.Key.Key_unknown
        if 0x41 <= vk <= 0x5A:           # A-Z
            qt_key = Qt.Key(vk)
        elif 0x30 <= vk <= 0x39:         # 0-9
            qt_key = Qt.Key(vk)
        elif 0x70 <= vk <= 0x87:         # F1-F24
            qt_key = Qt.Key(Qt.Key.Key_F1.value + (vk - 0x70))

        if qt_key == Qt.Key.Key_unknown:
            return True  # 无法映射，只拦截

        fake_event = QKeyEvent(
            QEvent.Type.KeyPress,
            qt_key,
            qt_mods,
            "",
        )
        self._line_edit.keyPressEvent(fake_event)
        return True

    def handle_mouse_hotkey(self, token: str, callback) -> bool:
        """录入侧键。

        侧键在录入期间被低级钩子整体吞掉，Qt 收不到 mousePressEvent，因此
        这条链是录入的唯一入口——无论指针当时落在录入框上还是窗口别处。
        侧键是原子值、不与修饰键叠加，按下即直接覆盖当前内容（包括框里可能
        正显示的 "ctrl+" 这类未完成的键盘组合前缀）。
        """
        self._line_edit.setText(token)
        return True

class HotkeyEdit(QWidget):
    """全局热键录入框：按键块 + 右侧状态图标（可用 / 未设置 / 冲突）。"""
    textChanged = Signal(str)

    def __init__(self, parent=None):
        super().__init__(parent)
        self._system_available = None
        self._validation_error = ""
        self.layout = QHBoxLayout(self)
        self.layout.setContentsMargins(0, 0, 0, 0)
        self.layout.setSpacing(dialog_scaled(STATUS_GAP))

        self.edit = _HotkeyLineEdit(self)
        self.layout.addWidget(self.edit, 1)

        self.status_icon = StatusIcon(self)
        self.layout.addWidget(self.status_icon, 0, Qt.AlignmentFlag.AlignVCenter)

        from core.shortcut_manager import HotkeySystem
        self.hotkey_system = HotkeySystem()

        # Debounce timer for checking availability
        self.check_timer = QTimer(self)
        self.check_timer.setSingleShot(True)
        self.check_timer.setInterval(200) # Check 200ms after input stops
        self.check_timer.timeout.connect(self._check_availability_now)

        # Connect internal edit signals
        self.edit.textChanged.connect(self._on_text_changed)
        self.edit.textChanged.connect(self.textChanged.emit)
        self._render_status()

    def setText(self, text):
        self.edit.setText(text)
        self._check_availability_now()

    def text(self):
        return self.edit.text()

    @property
    def status_state(self) -> str:
        """状态图标当前的状态："ok"、"idle"、"error"，空串表示不显示。"""
        return self.status_icon.state

    def _on_text_changed(self, text):
        # 文本一变，旧值的系统检测结果就失效。设置页级冲突状态由外部
        # textChanged 监听器随后更新，因此这里先清掉旧的可用性显示。
        self._system_available = None
        self._render_status()
        # If hotkey is complete (not ending with +), verify it
        if text and not text.endswith("+"):
            self.check_timer.start()
        else:
            self.check_timer.stop()

    def _check_availability_now(self):
        hotkey = self.text().strip()
        if not hotkey:
            self._system_available = True  # 空值表示未绑定，是合法状态
            self._render_status()
            return True
        if hotkey.endswith("+"):
            self._system_available = False
            self._render_status()
            return False

        # Check availability using our HotkeySystem
        self._system_available = self.hotkey_system.check_hotkey_availability(hotkey)
        self._render_status()
        return self._system_available

    def _render_status(self):
        """合并设置页级校验与系统占用检测，设置页级错误优先显示。"""
        hotkey = self.text().strip()
        if self._validation_error:
            state, tip = "error", self._validation_error
        elif not hotkey:
            state, tip = "idle", _tr("Not set")
        elif hotkey.endswith("+") or self._system_available is None:
            state, tip = "", ""
        elif self._system_available:
            state, tip = "ok", _tr("Available")
        else:
            state, tip = "error", _tr("Already in use by another app.")
        self.status_icon.setState(state, tip)
        self.edit.setErrorState(state == "error")
        self.edit.setToolTip(tip if state == "error" else "")

    def set_validation_error(self, message: str = ""):
        """设置由容器统一校验得出的错误（例如多个业务重复绑定）。"""
        self._validation_error = message or ""
        self._render_status()

    @property
    def validation_error(self) -> str:
        """当前的容器级错误文案，空串表示这一格没问题。"""
        return self._validation_error

    def validate_now(self) -> bool:
        """同步验证当前值，供设置窗口在落盘前做最终检查。"""
        if self._validation_error:
            return False
        return bool(self._check_availability_now())


def validate_hotkey_group(edits, *, check_system: bool = False) -> bool:
    """把一组录入框当作同一个冲突域整体校验，并把结果写回每个录入框。

    同一个实际按键不能同时绑给两个业务，否则第二个注册必定失败——键盘热键
    是 RegisterHotKey 拒绝重复注册，鼠标侧键则是登记表拒绝，两种都只会静默
    失败，所以冲突必须在界面上拦下来。留空和未录完的前缀不参与判重。

    Args:
        edits: 录入框序列，顺序不影响结果。
        check_system: 是否同时跑系统占用检测。落盘前用它绕过 200ms 防抖，
            确保拿到的是当前值的最终结果而不是上一次的残留。

    Returns:
        整组是否全部可用。
    """
    edits = list(edits)
    by_identity = {}
    for edit in edits:
        identity = hotkey_identity(edit.text())
        if identity is not None:
            by_identity.setdefault(identity, []).append(edit)

    duplicated = {
        edit
        for same_key_edits in by_identity.values()
        if len(same_key_edits) > 1
        for edit in same_key_edits
    }
    conflict_message = _tr("This hotkey is assigned more than once.")
    for edit in edits:
        edit.set_validation_error(conflict_message if edit in duplicated else "")

    if not check_system:
        return not duplicated

    # 先整体跑一遍再判断：all() 会在第一个 False 处短路，那样后面的录入框拿不到
    # 新的检测结果，红叉只能一次暴露一个，用户得反复点应用才能找齐。
    system_results = [edit.validate_now() for edit in edits]
    return all(system_results) and not duplicated


class _HotkeyLineEdit(KeyChipLineEdit):
    """
    The internal customized QLineEdit that captures hotkeys.
    (This is the original HotkeyEdit logic, renamed to be internal)
    """
    def __init__(self, parent=None):
        super().__init__(parent)
        # NOTE: Do NOT set ReadOnly=True, as it may block key events on some platforms/versions.
        # We will block standard input by not calling super().keyPressEvent().
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)

        # 创建 handler，聚焦时注册，失焦时注销
        self._shortcut_handler = _HotkeyEditHandler(self)
        # 侧键独占是引用计数式的，这里记录本控件是否已经占了一份，
        # 保证 focusIn / focusOut 成对增减，不会漏放或重复放。
        self._mouse_capture_held = False

    @safe_event
    def keyPressEvent(self, event: QKeyEvent):
        key = event.key()
        modifiers = event.modifiers()
        
        # Accept the event to prevent propagation/default handling
        event.accept()

        # 2. Check for modifiers
        parts = []
        
        # Check modifiers from event state
        if modifiers & Qt.KeyboardModifier.ControlModifier:
            parts.append("Ctrl")
        if modifiers & Qt.KeyboardModifier.ShiftModifier:
            parts.append("Shift")
        if modifiers & Qt.KeyboardModifier.AltModifier:
            parts.append("Alt")
        if modifiers & Qt.KeyboardModifier.MetaModifier:
            parts.append("Win")

        # Handle case where the pressed key IS a modifier but 'modifiers' flags might be lagging or different context
        # (Though usually modifiers() is accurate for combo, let's be safe if user just pressed Ctrl)
        if key == Qt.Key.Key_Control and "Ctrl" not in parts: parts.append("Ctrl")
        if key == Qt.Key.Key_Shift and "Shift" not in parts: parts.append("Shift")
        if key == Qt.Key.Key_Alt and "Alt" not in parts: parts.append("Alt")
        if key == Qt.Key.Key_Meta and "Win" not in parts: parts.append("Win")

        is_modifier_present = len(parts) > 0
        
        # Check if the current key is a modifier key itself
        is_current_key_modifier = key in (Qt.Key.Key_Control, Qt.Key.Key_Shift, Qt.Key.Key_Alt, Qt.Key.Key_Meta)

        # 3. Get key text (only if not a modifier key)
        key_text = ""
        if not is_current_key_modifier:
            _display_map = get_key_display_map()
            if key in _display_map:
                key_text = _display_map[key]
            elif Qt.Key.Key_F1 <= key <= Qt.Key.Key_F35:
                key_text = f"F{key - Qt.Key.Key_F1 + 1}"
            else:
                # For letters and numbers
                text = QKeySequence(key).toString(QKeySequence.SequenceFormat.PortableText)
                if not text:
                    text = event.text().upper()
                key_text = text
        
        # 4. Update Display
        
        # If basically nothing valid/known pressed (and no modifiers), ignore or clear?
        if not is_modifier_present and not key_text:
             # e.g. some unknown key
             return

        # Special Case: clearing with Backspace/Delete (without modifiers)
        if not is_modifier_present and key_text in ("Backspace", "Delete"):
            self.setText("")
            return

        # 5. Validation / Construction
        
        # If we have modifiers but no main key yet -> Show "Ctrl + "
        if is_modifier_present and not key_text:
             hotkey_str = "+".join(parts).lower() + "+"
             self.setText(hotkey_str)
             return

        # If we have a main key
        # Rule: Valid = (Modifiers + Key) OR (Function Key) OR (Special Key)
        
        is_function_key = (Qt.Key.Key_F1 <= key <= Qt.Key.Key_F35)
        special_keys = [
            Qt.Key.Key_Home, Qt.Key.Key_End, Qt.Key.Key_PageUp, Qt.Key.Key_PageDown,
            Qt.Key.Key_Left, Qt.Key.Key_Right, Qt.Key.Key_Up, Qt.Key.Key_Down,
            Qt.Key.Key_Insert, Qt.Key.Key_Print, Qt.Key.Key_Pause, 
        ]
        is_special_key = (key in special_keys)
        
        is_valid = False
        if is_modifier_present:
            is_valid = True
        elif is_function_key or is_special_key:
            is_valid = True
        
        if is_valid:
            parts.append(key_text)
            hotkey_str = "+".join(parts).lower()
            self.setText(hotkey_str)
        else:
             # Just a letter 'A' without modifiers -> Invalid
             # But maybe we want to show it temporarily? 
             # Implementation choice: User said "check valid then show".
             pass

    @safe_event
    def keyReleaseEvent(self, event: QKeyEvent):
        """Handle key release to cleanup 'dangling' modifiers display"""
        # If the text ends with '+', it means we are in the middle of a combo
        # If user releases a modifier, we should update the display.
        # But determining exactly what remains pressed can be tricky with just ReleaseEvent.
        # Simple approach: If text matches "ctrl+", and ctrl is released, clear it?
        
        key = event.key()
        
        # Only care if a modifier was released
        if key in (Qt.Key.Key_Control, Qt.Key.Key_Shift, Qt.Key.Key_Alt, Qt.Key.Key_Meta):
            curr_text = self.text()
            if curr_text.endswith("+"):
                # We were in an incomplete state.
                # Check if ANY modifiers are still held?
                # event.modifiers() should reflect state AFTER release (or before? Qt varies).
                # Usually modifiers() in release event reflects the remaining modifiers.
                
                modifiers = event.modifiers()
                parts = []
                if modifiers & Qt.KeyboardModifier.ControlModifier: parts.append("Ctrl")
                if modifiers & Qt.KeyboardModifier.ShiftModifier: parts.append("Shift")
                if modifiers & Qt.KeyboardModifier.AltModifier: parts.append("Alt")
                if modifiers & Qt.KeyboardModifier.MetaModifier: parts.append("Win")

                if parts:
                    self.setText("+".join(parts).lower() + "+")
                else:
                    # No modifiers left, and we were incomplete -> Revert to empty or previous?
                    # User said "invalid input is empty".
                    self.setText("")
        
        super().keyReleaseEvent(event)

    @safe_event
    def focusInEvent(self, event):
        super().focusInEvent(event)
        self._shortcut_handler._active = True
        manager = ShortcutManager.instance()
        manager.register(self._shortcut_handler)
        if not self._mouse_capture_held:
            # 让低级钩子在录入期间独占侧键，这样尚未绑定的侧键也录得到。
            self._mouse_capture_held = True
            manager.begin_mouse_capture()

    @safe_event
    def focusOutEvent(self, event):
        self._shortcut_handler._active = False
        manager = ShortcutManager.instance()
        manager.unregister(self._shortcut_handler)
        if self._mouse_capture_held:
            # 必须成对释放，否则钩子一直挂着，其它程序永远拿不回侧键。
            self._mouse_capture_held = False
            manager.end_mouse_capture()
        super().focusOutEvent(event)

    def contextMenuEvent(self, event):
        # Disable context menu to prevent pasting invalid text
        pass
 
