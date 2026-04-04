# -*- coding: utf-8 -*-
"""
应用内快捷键录入框

与 HotkeyEdit 的区别：
- 不检查系统热键冲突（不显示 ✅/❌）
- 允许单个字母键（如 "C"）作为合法输入
- 允许 Shift+字母 组合（如 "Shift+C"）
- 样式更紧凑
"""
from PySide6.QtWidgets import QLineEdit
from PySide6.QtCore import Qt
from PySide6.QtGui import QKeyEvent, QKeySequence

from core.shortcut_manager import get_key_display_map
from core import safe_event


class InAppKeyEdit(QLineEdit):
    """应用内快捷键录入框 — 点击后按下想要的键组合即可"""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setReadOnly(True)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.setPlaceholderText("...")

    # ── 键盘事件 ──────────────────────────────────────────

    @safe_event
    def keyPressEvent(self, event: QKeyEvent):
        key = event.key()
        mods = event.modifiers()
        event.accept()

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

        # Backspace / Delete（无修饰键）→ 清空
        if not parts and key in (Qt.Key.Key_Backspace, Qt.Key.Key_Delete):
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
 