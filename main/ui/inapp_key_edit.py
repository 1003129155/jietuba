# -*- coding: utf-8 -*-
"""
应用内快捷键录入框

与 HotkeyEdit 的区别：
- 不检查系统热键冲突（没有状态图标）
- 允许单个字母键（如 "C"）作为合法输入
- 允许 Shift+字母 组合（如 "Shift+C"）
"""
from PySide6.QtCore import Qt
from PySide6.QtGui import QKeyEvent, QKeySequence

from core.shortcut_manager import MOUSE_BUTTON_MIDDLE, get_key_display_map
from core import safe_event

from .key_chip import KeyChipLineEdit


class InAppKeyEdit(KeyChipLineEdit):
    """应用内快捷键录入框 — 点击后按下想要的键组合即可（也可按鼠标中键）"""

    # ShortcutManager 的应用内鼠标分发看这个标记来放行：不放行的话，指针只要
    # 恰好停在某个钉图上方，中键就会先被钉图 handler 消费掉，这里永远录不到。
    # 键盘那侧靠 _is_text_input_active 达到同样效果，鼠标没有对应机制。
    _captures_inapp_mouse_shortcut = True

    def __init__(self, parent=None, *, allow_mouse=True):
        super().__init__(parent)
        self._allow_mouse = allow_mouse
        self.setReadOnly(True)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)

    # ── 鼠标事件 ──────────────────────────────────────────

    @safe_event
    def mousePressEvent(self, event):
        """中键录成绑定，其余按键交还给按键块（左键获取焦点或点 × 清除）。

        只做中键：侧键走的是全局热键那套低级钩子，Qt 这里根本收不到；左右键
        在应用里到处都有用途，录成快捷键会让界面没法操作。
        """
        if event.button() != Qt.MouseButton.MiddleButton:
            super().mousePressEvent(event)
            return
        event.accept()
        if not self._allow_mouse:
            return
        self.setFocus(Qt.FocusReason.MouseFocusReason)
        parts = []
        mods = event.modifiers()
        if mods & Qt.KeyboardModifier.ControlModifier:
            parts.append("Ctrl")
        if mods & Qt.KeyboardModifier.ShiftModifier:
            parts.append("Shift")
        if mods & Qt.KeyboardModifier.AltModifier:
            parts.append("Alt")
        parts.append(MOUSE_BUTTON_MIDDLE)
        # 存进配置的就是这段文本，所以必须是解析器认识的形态（全小写）
        self.setText("+".join(parts).lower())

    # ── 键盘事件 ──────────────────────────────────────────

    @safe_event
    def keyPressEvent(self, event: QKeyEvent):
        key = event.key()
        mods = event.modifiers()
        event.accept()

        # Escape is the fixed screenshot cancel key and cannot be rebound.
        if key == Qt.Key.Key_Escape:
            self.setText("")
            return

        parts: list[str] = []
        if mods & Qt.KeyboardModifier.ControlModifier:
            parts.append("Ctrl")
        if mods & Qt.KeyboardModifier.ShiftModifier:
            parts.append("Shift")
        if mods & Qt.KeyboardModifier.AltModifier:
            parts.append("Alt")

        is_modifier = key in (
            Qt.Key.Key_Control, Qt.Key.Key_Shift,
            Qt.Key.Key_Alt, Qt.Key.Key_Meta,
        )

        # 仅按下修饰键 → 显示中间状态 "Ctrl+"
        if is_modifier:
            if parts:
                self.setText("+".join(parts).lower() + "+")
            return

        # Backspace（无修饰键）→ 清空
        if not parts and key == Qt.Key.Key_Backspace:
            self.setText("")
            return

        # 获取主键文字
        key_text = self._key_to_text(key)
        if not key_text:
            return

        parts.append(key_text)
        self.setText("+".join(parts).lower())

    @safe_event
    def keyReleaseEvent(self, event: QKeyEvent):
        key = event.key()
        if key in (Qt.Key.Key_Control, Qt.Key.Key_Shift,
                   Qt.Key.Key_Alt, Qt.Key.Key_Meta):
            if self.text().endswith("+"):
                # 修饰键松开而未完成组合 → 清空
                remaining = event.modifiers()
                parts = []
                if remaining & Qt.KeyboardModifier.ControlModifier:
                    parts.append("Ctrl")
                if remaining & Qt.KeyboardModifier.ShiftModifier:
                    parts.append("Shift")
                if remaining & Qt.KeyboardModifier.AltModifier:
                    parts.append("Alt")
                if parts:
                    self.setText("+".join(parts).lower() + "+")
                else:
                    self.setText("")
        super().keyReleaseEvent(event)

    def contextMenuEvent(self, event):
        pass  # 禁止右键菜单

    # ── 内部 ──────────────────────────────────────────────

    @staticmethod
    def _key_to_text(key: int) -> str:
        display_map = get_key_display_map()
        if key in display_map:
            return display_map[key]
        if Qt.Key.Key_F1 <= key <= Qt.Key.Key_F24:
            return f"F{key - Qt.Key.Key_F1 + 1}"
        text = QKeySequence(key).toString(QKeySequence.SequenceFormat.PortableText)
        return text if text else ""
