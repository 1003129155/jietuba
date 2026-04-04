"""快捷键录入框组件 - 系统快捷键冲突检测

用户按下按键时实时检测是否与系统全局快捷键冲突，
提供视觉反馈，防止注册冲突的快捷键。
"""

from PySide6.QtWidgets import QLineEdit, QWidget, QHBoxLayout, QLabel
from PySide6.QtCore import Qt, QTimer
from PySide6.QtGui import QKeyEvent, QKeySequence

from core.shortcut_manager import ShortcutManager, ShortcutHandler, get_key_display_map
from core import safe_event


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

class HotkeyEdit(QWidget):
    """
    A composite widget that contains a QLineEdit for capturing hotkeys
    and a validation status indicator (Check/X icon).
    """
    def __init__(self, parent=None):
        super().__init__(parent)
        self.layout = QHBoxLayout(self)
        self.layout.setContentsMargins(0, 0, 0, 0)
        self.layout.setSpacing(5)

        self.edit = _HotkeyLineEdit(self)
        self.layout.addWidget(self.edit)

        # Status indicator
        self.status_lbl = QLabel()
        self.status_lbl.setContextMenuPolicy(Qt.ContextMenuPolicy.NoContextMenu)
        self.status_lbl.setFixedWidth(20)  # Reserve space
        self.status_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)  # Center icon
        self.layout.addWidget(self.status_lbl)
        
        from core.shortcut_manager import HotkeySystem
        self.hotkey_system = HotkeySystem()
        
        # Debounce timer for checking availability
        self.check_timer = QTimer(self)
        self.check_timer.setSingleShot(True)
        self.check_timer.setInterval(200) # Check 200ms after input stops
        self.check_timer.timeout.connect(self._check_availability_now)

        # Connect internal edit signals
        self.edit.textChanged.connect(self._on_text_changed)

    def setText(self, text):
        self.edit.setText(text)
        self._check_availability_now()

    def text(self):
        return self.edit.text()
    
    def setPlaceholderText(self, text):
        self.edit.setPlaceholderText(text)
    
    def setFixedWidth(self, w):
        # Apply width to the composite widget or the edit?
        # Usually user calls this on the editor. Let's redirect to super but ensure layout handles it.
        super().setFixedWidth(w)

    def setStyleSheet(self, style):
        # Forward stylesheet to the line edit if possible, or just self
        self.edit.setStyleSheet(style)

    def _on_text_changed(self, text):
        # If hotkey is complete (not ending with +), verify it
        if text and not text.endswith("+"):
            self.check_timer.start()
        else:
            self.status_lbl.clear()

    def _check_availability_now(self):
        hotkey = self.text().strip()
        if not hotkey or hotkey.endswith("+"):
             self.status_lbl.clear()
             return

        # Check availability using our HotkeySystem
        is_available = self.hotkey_system.check_hotkey_availability(hotkey)

        if is_available:
            self.status_lbl.setText("✅")
            self.status_lbl.setToolTip("Hotkey is available")
            self.status_lbl.setStyleSheet("color: green; font-weight: bold;")
        else:
            self.status_lbl.setText("❌") 
            self.status_lbl.setToolTip("Hotkey is already in use by system or other app")
            self.status_lbl.setStyleSheet("color: red; font-weight: bold;")


class _HotkeyLineEdit(QLineEdit):
    """
    The internal customized QLineEdit that captures hotkeys.
    (This is the original HotkeyEdit logic, renamed to be internal)
    """
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setPlaceholderText("Press shortcut...")
        # NOTE: Do NOT set ReadOnly=True, as it may block key events on some platforms/versions.
        # We will block standard input by not calling super().keyPressEvent().
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self.setStyleSheet("""
            QLineEdit {
                border: 1px solid #C8D0DA;
                border-radius: 6px;
                padding: 4px 8px;
                background: #FFFFFF;
                color: #1A1A2E;
                font-size: 13px;
            }
            QLineEdit:focus {
                border: 2px solid #2196F3;
                background: #F0F7FF;
            }
            QLineEdit:hover {
                border-color: #90CAF9;
            }
        """)

        # 创建 handler，聚焦时注册，失焦时注销
        self._shortcut_handler = _HotkeyEditHandler(self)

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
        ShortcutManager.instance().register(self._shortcut_handler)

    @safe_event
    def focusOutEvent(self, event):
        self._shortcut_handler._active = False
        ShortcutManager.instance().unregister(self._shortcut_handler)
        super().focusOutEvent(event)

    def contextMenuEvent(self, event):
        # Disable context menu to prevent pasting invalid text
        pass
 