# -*- coding: utf-8 -*-
"""One-hotkey selected-text probe and translation window router."""

from __future__ import annotations

import ctypes

from PySide6.QtCore import QObject, QPoint, QTimer, Slot
from PySide6.QtGui import QCursor
from PySide6.QtWidgets import QApplication

from core.logger import log_debug, log_exception, T
from settings import get_tool_settings_manager


VK_CONTROL = 0x11
VK_INSERT = 0x2D
KEYEVENTF_KEYUP = 0x0002


class SmartTranslationController(QObject):
    """Probe selected text and select one of the compact popup's two modes."""

    COPY_DELAY_MS = 55
    FALLBACK_READ_DELAY_MS = 120
    PROBE_TIMEOUT_MS = 500

    def __init__(self, parent: QObject | None = None):
        super().__init__(parent)
        self._config = get_tool_settings_manager()
        self._probe_active = False
        self._copy_dispatched = False
        self._probe_token = 0
        self._cursor_position = QPoint()

    @property
    def probe_active(self) -> bool:
        return self._probe_active

    @Slot()
    def trigger(self) -> None:
        """Begin one non-blocking selection probe."""
        self._probe_token += 1
        token = self._probe_token
        self._probe_active = True
        self._copy_dispatched = False
        self._cursor_position = QCursor.pos()
        log_debug(T("智能翻译探测开始: token={token}", token=token), "Translation")

        QTimer.singleShot(self.COPY_DELAY_MS, lambda: self._dispatch_copy(token))
        QTimer.singleShot(self.PROBE_TIMEOUT_MS, lambda: self._on_timeout(token))

    def translate_selection(self, text: str) -> None:
        """Translate text supplied by a caller that owns its own selection.

        Used by widgets whose selection cannot be reached through the system
        clipboard shortcut (self-drawn text layers such as the pinned-image OCR
        overlay). Cancels any in-flight probe so its timers become no-ops.
        """
        text = text.strip() if isinstance(text, str) else ""
        if not text:
            return
        self._probe_token += 1
        self._probe_active = False
        self._copy_dispatched = False
        self._cursor_position = QCursor.pos()
        log_debug(T("外部选区直接翻译: {char_count} 字符", char_count=len(text)), "Translation")
        self._open_compact(text)

    def _dispatch_copy(self, token: int) -> None:
        if not self._is_current(token):
            return
        self._copy_dispatched = True
        try:
            self._send_copy_shortcut()
            if not self._clipboard_monitor_available():
                QTimer.singleShot(
                    self.FALLBACK_READ_DELAY_MS,
                    lambda: self._read_clipboard_fallback(token),
                )
        except Exception as exc:
            log_exception(exc, T("发送智能翻译复制快捷键"))
            self._open_compact_input(token, "copy-error")

    def _clipboard_monitor_available(self) -> bool:
        """Return whether the shared clipboard history event stream is active."""
        app = self.parent()
        manager = getattr(app, "clipboard_manager", None)
        if manager is None or not getattr(manager, "is_available", False):
            return False
        try:
            return bool(manager.is_monitoring())
        except Exception:
            return False

    def _read_clipboard_fallback(self, token: int) -> None:
        """Read once when clipboard history monitoring is intentionally unavailable."""
        if not self._is_current(token):
            return
        text = QApplication.clipboard().text().strip()
        if text:
            self._probe_active = False
            log_debug(
                T(
                    "剪贴板监听未启用，直接读取当前文本: {char_count} 字符",
                    char_count=len(text),
                ),
                "Translation",
            )
            self._open_compact(text)
            return
        self._open_compact_input(token, "empty-clipboard")

    def _send_copy_shortcut(self) -> None:
        """Send the Windows alternate copy shortcut without raising console SIGINT."""
        user32 = ctypes.windll.user32
        user32.keybd_event(VK_CONTROL, 0, 0, 0)
        user32.keybd_event(VK_INSERT, 0, 0, 0)
        user32.keybd_event(VK_INSERT, 0, KEYEVENTF_KEYUP, 0)
        user32.keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0)

    @Slot(object)
    def on_clipboard_item(self, item) -> None:
        """Consume only the clipboard event belonging to an active probe."""
        if not self._probe_active or not self._copy_dispatched:
            return

        token = self._probe_token
        content_type = getattr(item, "content_type", "")
        text = getattr(item, "content", "") if content_type == "text" else ""
        text = text.strip() if isinstance(text, str) else ""

        if content_type == "text" and text:
            self._probe_active = False
            log_debug(T("智能翻译获取文本成功: {char_count} 字符", char_count=len(text)), "Translation")
            self._open_compact(text)
            return

        log_debug(
            T(
                "智能翻译忽略非文本内容: {content_type_display}",
                content_type_display=content_type or "unknown",
            ),
            "Translation",
        )
        self._open_compact_input(token, "non-text")

    def _on_timeout(self, token: int) -> None:
        if not self._is_current(token):
            return
        self._open_compact_input(token, "timeout")

    def _is_current(self, token: int) -> bool:
        return self._probe_active and token == self._probe_token

    def _translation_manager(self):
        from .translation_manager import TranslationManager

        return TranslationManager.instance()

    def _translation_params(self) -> dict:
        getter = getattr(
            self._config,
            "get_translation_request_params",
            self._config.get_translation_params,
        )
        return getter()

    def _open_compact(self, text: str) -> None:
        params = self._translation_params()
        self._translation_manager().translate_compact(
            text=text,
            position=self._cursor_position,
            **params,
        )

    def _open_compact_input(self, token: int, reason: str) -> None:
        if not self._is_current(token):
            return
        self._probe_active = False
        log_debug(T("智能翻译转入小窗手动输入: {reason}", reason=reason), "Translation")
        params = self._translation_params()
        self._translation_manager().open_compact_input(
            position=self._cursor_position,
            **params,
        )
