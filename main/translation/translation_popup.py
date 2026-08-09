# -*- coding: utf-8 -*-
"""Compact translation popup: one always-editable window for every entry point."""

from __future__ import annotations

from PySide6.QtCore import QPoint, QPropertyAnimation, QRectF, Qt, QTimer, Signal
from PySide6.QtGui import QColor, QCursor, QFont, QPainter, QPen
from PySide6.QtWidgets import (
    QApplication,
    QFrame,
    QHBoxLayout,
    QLabel,
    QMenu,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

from core.i18n import make_tr
from ui.fluent_lite import TextEdit
from .translation_dialog import DARK, LIGHT, Palette
from .languages import TRANSLATION_LANGUAGES


_tr = make_tr("TranslationDialog")


class TranslationPopup(QWidget):
    """Single compact window used for both selected text and typed input.

    The original-text box is always editable; the only difference between entry
    points is whether the popup takes keyboard focus on show (``activate``).
    Selection translation must not steal focus, otherwise the user's selection
    and caret in the source application are lost.
    """

    open_full_requested = Signal(str, str, str)
    manual_translate_requested = Signal(str)
    manual_input_changed = Signal()
    target_lang_changed = Signal(str)  # 目标语言变更信号

    WIDTH = 420
    MIN_HEIGHT = 176
    MAX_HEIGHT = 520
    SOURCE_MIN_HEIGHT = 44
    SOURCE_MAX_HEIGHT = 120
    RESULT_MIN_HEIGHT = 44
    RESULT_MAX_HEIGHT = 208

    def __init__(self, parent: QWidget | None = None):
        flags = (
            Qt.WindowType.Tool
            | Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.WindowStaysOnTopHint
        )
        super().__init__(parent, flags)
        self._palette: Palette = DARK
        self._source_text = ""
        self._translated_text = ""
        self._error_text = ""
        # Guards textChanged while the popup fills the box programmatically.
        self._suppress_auto = False
        self._backend_ready = True
        self._drag_offset: QPoint | None = None
        self._loading_step = 0
        self._target_lang = "ZH"  # 当前目标语言

        self.setObjectName("translationPopup")
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, True)
        self.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating, True)
        self.setFont(QFont("Microsoft YaHei UI", 10))
        self.setFixedWidth(self.WIDTH)
        self.setMinimumHeight(self.MIN_HEIGHT)

        self._build_ui()
        self.set_theme("dark")

        self._loading_timer = QTimer(self)
        self._loading_timer.setInterval(320)
        self._loading_timer.timeout.connect(self._advance_loading)

        self._manual_debounce = QTimer(self)
        self._manual_debounce.setSingleShot(True)
        self._manual_debounce.setInterval(450)
        self._manual_debounce.timeout.connect(self._request_manual_translation)

        self._fade = QPropertyAnimation(self, b"windowOpacity", self)
        self._fade.setDuration(140)

    def _build_ui(self) -> None:
        root = QVBoxLayout(self)
        root.setContentsMargins(12, 10, 12, 11)
        root.setSpacing(8)

        header = QHBoxLayout()
        # Left margin 0 keeps the badge pill flush with the input box below it.
        header.setContentsMargins(0, 0, 0, 0)
        header.setSpacing(8)

        # No window title: the engine badge already identifies the popup, and
        # the empty space next to it still works as the drag handle.
        self.backend_badge = QLabel(_tr("Engine not configured"), self)
        self.backend_badge.setObjectName("popupBadge")
        self.backend_badge.setFixedHeight(21)
        header.addWidget(self.backend_badge)
        header.addStretch(1)

        self.close_button = QPushButton("×", self)
        self.close_button.setObjectName("popupClose")
        self.close_button.setCursor(Qt.CursorShape.PointingHandCursor)
        self.close_button.setFixedSize(28, 26)
        self.close_button.setToolTip(_tr("Close"))
        self.close_button.clicked.connect(self._hide_popup)
        header.addWidget(self.close_button)
        root.addLayout(header)

        self.source_edit = self._make_text_view("popupSource", editable=True)
        self.source_edit.setMaximumHeight(self.SOURCE_MAX_HEIGHT)
        self.source_edit.setPlaceholderText(_tr("Enter text to translate..."))
        self.source_edit.textChanged.connect(self._on_source_changed)
        root.addWidget(self.source_edit)

        self.divider = QFrame(self)
        self.divider.setObjectName("popupDivider")
        self.divider.setFixedHeight(1)
        root.addWidget(self.divider)

        self.result_edit = self._make_text_view("popupResult")
        self.result_edit.setMaximumHeight(self.RESULT_MAX_HEIGHT)
        root.addWidget(self.result_edit)

        footer = QHBoxLayout()
        footer.setContentsMargins(0, 1, 0, 0)
        footer.setSpacing(7)

        # 目标语言标签
        lang_label = QLabel(_tr("Target") + ": ", self)
        lang_label.setObjectName("popupLangLabel")
        footer.addWidget(lang_label)

        self.lang_button = QPushButton("中文", self)
        self.lang_button.setObjectName("popupLangButton")
        self.lang_button.setCursor(Qt.CursorShape.PointingHandCursor)
        self.lang_button.setToolTip(_tr("Switch target language"))
        self.lang_button.clicked.connect(self._show_lang_menu)
        footer.addWidget(self.lang_button)
        
        footer.addStretch(1)

        self.copy_button = QPushButton(_tr("Copy Translation"), self)
        self.copy_button.setObjectName("popupChip")
        self.copy_button.setCursor(Qt.CursorShape.PointingHandCursor)
        self.copy_button.clicked.connect(self._copy_translation)
        footer.addWidget(self.copy_button)

        self.full_button = QPushButton(_tr("Open full window"), self)
        self.full_button.setObjectName("popupPrimary")
        self.full_button.setCursor(Qt.CursorShape.PointingHandCursor)
        self.full_button.clicked.connect(self._open_full)
        footer.addWidget(self.full_button)
        root.addLayout(footer)

    def _make_text_view(self, object_name: str, *, editable: bool = False) -> TextEdit:
        view = TextEdit(self)
        view.setObjectName(object_name)
        view.setReadOnly(not editable)
        view.setAcceptRichText(False)
        view.setFrameShape(QFrame.Shape.NoFrame)
        view.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        view.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAsNeeded)
        view.setTextInteractionFlags(
            Qt.TextInteractionFlag.TextEditorInteraction
            if editable
            else Qt.TextInteractionFlag.TextSelectableByMouse
        )
        return view

    def _show_lang_menu(self) -> None:
        """显示语言选择菜单"""
        menu = QMenu(self)
        menu.setObjectName("popupLangMenu")
        
        # 添加所有支持的语言
        for code, name in TRANSLATION_LANGUAGES.items():
            action = menu.addAction(name)
            action.setData(code)
            if code == self._target_lang:
                action.setCheckable(True)
                action.setChecked(True)
        
        # 在按钮下方弹出菜单
        pos = self.lang_button.mapToGlobal(self.lang_button.rect().bottomLeft())
        action = menu.exec(pos)
        
        if action:
            new_lang = action.data()
            if new_lang != self._target_lang:
                self._target_lang = new_lang
                self.lang_button.setText(TRANSLATION_LANGUAGES[new_lang])
                self.target_lang_changed.emit(new_lang)
                # 如果有原文，立即重新翻译到新语言
                if self._source_text:
                    self._enter_loading()
                    self.manual_translate_requested.emit(self._source_text)

    def set_target_lang(self, lang_code: str) -> None:
        """设置目标语言（外部调用）"""
        if lang_code in TRANSLATION_LANGUAGES:
            self._target_lang = lang_code
            self.lang_button.setText(TRANSLATION_LANGUAGES[lang_code])

    def show_popup(
        self,
        source_text: str = "",
        position: QPoint | None = None,
        *,
        activate: bool = False,
    ) -> None:
        """Show the popup, optionally pre-filled with text to translate.

        Args:
            source_text: Text to place in the original box. Empty means the user
                will type it, so the result area stays hidden until then.
            position: Anchor point; defaults to the cursor.
            activate: ``True`` moves keyboard focus into the popup. Keep it
                ``False`` for selection translation so the source application
                keeps its focus and selection.
        """
        self._loading_timer.stop()
        self._manual_debounce.stop()
        self._source_text = source_text.strip()
        self._translated_text = ""
        self._error_text = ""
        self.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating, not activate)
        self._set_source_text(self._source_text)
        if self._source_text:
            # The caller starts the request right after showing the popup.
            self._enter_loading()
        else:
            self._hide_result()
        self._fit_content()
        self._place_near(position or QCursor.pos())
        self._show_animated()
        if activate:
            self.activateWindow()
            QTimer.singleShot(0, self._focus_source_input)

    def _set_source_text(self, text: str) -> None:
        """Fill the original box without triggering auto-translation."""
        self._suppress_auto = True
        try:
            self.source_edit.setPlainText(text)
        finally:
            self._suppress_auto = False

    def _enter_loading(self) -> None:
        """Reveal the result area in its pending state."""
        self.divider.show()
        self.result_edit.show()
        self.copy_button.show()
        self.copy_button.setEnabled(False)
        self.result_edit.setProperty("error", False)
        self.result_edit.setPlainText(_tr("Translating..."))
        self._refresh_result_style()
        self._loading_step = 0
        self._loading_timer.start()

    def _hide_result(self) -> None:
        """Collapse the result area while there is nothing to translate."""
        self._loading_timer.stop()
        self.result_edit.clear()
        self.result_edit.setProperty("error", False)
        self._refresh_result_style()
        self.divider.hide()
        self.result_edit.hide()
        self.copy_button.hide()

    def _focus_source_input(self) -> None:
        if not self.isVisible():
            return
        try:
            self.source_edit.setFocus()
        except RuntimeError:
            # The popup may have been closed during the queued focus handoff.
            return

    def show_result(self, translated_text: str, detected_lang: str = "") -> None:
        self._loading_timer.stop()
        self._translated_text = translated_text
        self._error_text = "" if translated_text else _tr("No translation result")
        self.result_edit.setProperty("error", False)
        self.result_edit.setPlainText(translated_text or _tr("No translation result"))
        self.divider.show()
        self.result_edit.show()
        self.copy_button.show()
        self.copy_button.setEnabled(bool(translated_text))
        self._refresh_result_style()
        self._fit_content()

    def show_error(self, message: str) -> None:
        self._loading_timer.stop()
        self._translated_text = ""
        self._error_text = message
        self.result_edit.setProperty("error", True)
        self.result_edit.setPlainText(message)
        self.divider.show()
        self.result_edit.show()
        self.copy_button.show()
        self.copy_button.setEnabled(False)
        self._refresh_result_style()
        self._fit_content()

    def set_backend_ready(self, ready: bool) -> None:
        """Backward-compatible helper for older callers."""
        self.set_backend_status("DeepL API", ready)

    def set_backend_status(self, name: str, ready: bool) -> None:
        self._backend_ready = ready
        self.backend_badge.setText(
            name if ready else _tr("Engine not configured")
        )
        self.backend_badge.setProperty("ready", ready)
        self.backend_badge.style().unpolish(self.backend_badge)
        self.backend_badge.style().polish(self.backend_badge)

    def set_theme(self, theme_name: str) -> None:
        self._palette = LIGHT if theme_name == "light" else DARK
        p = self._palette
        self.setStyleSheet(
            f"""
            QWidget#translationPopup {{ background: transparent; color: {p.text}; }}
            QLabel#popupBadge {{
                color: {p.green}; background: {p.accent_tint}; border-radius: 7px;
                padding: 0 8px; font-size: 11px; font-weight: 600;
            }}
            QLabel#popupBadge[ready="false"] {{ color: {p.text_2}; background: {p.fill}; }}
            QPushButton#popupClose {{
                color: {p.text_2}; background: transparent; border: none;
                border-radius: 8px; font-size: 19px;
            }}
            QPushButton#popupClose:hover {{ color: white; background: {p.danger}; }}
            QTextEdit#popupSource {{
                color: {p.text}; background: {p.field};
                border: 1px solid {p.fill_hover}; border-radius: 9px;
                padding: 7px 9px; font-size: 14px;
            }}
            QTextEdit#popupSource:focus {{ border-color: {p.accent}; }}
            QTextEdit#popupResult {{
                color: {p.text}; background: transparent; border: none;
                padding: 2px 3px; font-size: 14px; font-weight: 550;
            }}
            QTextEdit#popupResult[error="true"] {{ color: {p.danger}; font-weight: 500; }}
            QFrame#popupDivider {{ background: {p.fill_hover}; border: none; }}
            QLabel#popupLangLabel {{
                color: {p.text_3}; font-size: 11px; padding-left: 3px;
            }}
            QPushButton#popupLangButton {{
                color: {p.text_2}; background: transparent; border: none;
                border-radius: 6px; padding: 4px 8px; font-size: 12px; font-weight: 600;
                text-align: left;
            }}
            QPushButton#popupLangButton:hover {{ 
                color: {p.text}; background: {p.fill}; 
            }}
            QPushButton#popupChip, QPushButton#popupPrimary {{
                border: none; border-radius: 8px; padding: 6px 10px;
                background: transparent; font-size: 12px; font-weight: 600;
            }}
            QPushButton#popupChip {{ color: {p.text_2}; }}
            QPushButton#popupChip:hover {{ color: {p.text}; background: {p.fill}; }}
            QPushButton#popupChip:disabled {{ color: {p.text_3}; background: transparent; }}
            QPushButton#popupPrimary {{ color: {p.accent}; }}
            QPushButton#popupPrimary:hover {{ background: {p.accent_tint}; }}
            QScrollBar:vertical {{ background: transparent; width: 5px; margin: 2px 0; }}
            QScrollBar::handle:vertical {{ background: {p.fill_hover}; border-radius: 2px; min-height: 20px; }}
            QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {{ height: 0; }}
            QMenu#popupLangMenu {{
                background: {p.surface_strong}; border: 1px solid {p.fill_hover};
                border-radius: 9px; padding: 5px;
            }}
            QMenu#popupLangMenu::item {{
                color: {p.text}; background: transparent; padding: 6px 12px;
                border-radius: 6px; font-size: 12px;
            }}
            QMenu#popupLangMenu::item:selected {{ background: {p.accent}; color: white; }}
            QMenu#popupLangMenu::item:checked {{ font-weight: 600; }}
            """
        )
        self.update()

    def _advance_loading(self) -> None:
        self._loading_step = (self._loading_step + 1) % 4
        base = _tr("Translating...").rstrip(".。…")
        self.result_edit.setPlainText(base + "." * self._loading_step)

    def _on_source_changed(self) -> None:
        if self._suppress_auto:
            return
        self._source_text = self.source_edit.toPlainText().strip()
        self._translated_text = ""
        self._error_text = ""
        self.manual_input_changed.emit()
        self._manual_debounce.stop()
        if not self._source_text:
            self._hide_result()
            self._fit_content()
            return
        if not self._backend_ready:
            self.show_error(_tr("API key not configured"))
            return
        self._manual_debounce.start()
        self._fit_content()

    def _request_manual_translation(self) -> None:
        text = self.source_edit.toPlainText().strip()
        if not text:
            return
        self._source_text = text
        self._translated_text = ""
        self._error_text = ""
        self._enter_loading()
        self._fit_content()
        self.manual_translate_requested.emit(text)

    def _refresh_result_style(self) -> None:
        self.result_edit.style().unpolish(self.result_edit)
        self.result_edit.style().polish(self.result_edit)

    def _fit_text_view(self, view: TextEdit, minimum: int, maximum: int) -> None:
        doc_height = int(view.document().size().height()) + 10
        view.setFixedHeight(max(minimum, min(doc_height, maximum)))

    def _fit_content(self) -> None:
        self._fit_text_view(
            self.source_edit, self.SOURCE_MIN_HEIGHT, self.SOURCE_MAX_HEIGHT
        )
        if self.result_edit.isVisible():
            self._fit_text_view(
                self.result_edit, self.RESULT_MIN_HEIGHT, self.RESULT_MAX_HEIGHT
            )
        self.layout().activate()
        desired = self.sizeHint().height()
        self.setFixedHeight(max(self.MIN_HEIGHT, min(desired, self.MAX_HEIGHT)))

    def _place_near(self, position: QPoint) -> None:
        screen = QApplication.screenAt(position) or QApplication.primaryScreen()
        if screen is None:
            self.move(position)
            return
        area = screen.availableGeometry()
        x = position.x() + 14
        y = position.y() + 22
        if x + self.width() > area.right() - 8:
            x = area.right() - self.width() - 8
        if y + self.height() > area.bottom() - 8:
            y = position.y() - self.height() - 16
        x = max(area.left() + 8, x)
        y = max(area.top() + 8, y)
        self.move(x, y)

    def _show_animated(self) -> None:
        self.setWindowOpacity(0.0)
        self.show()
        self.raise_()
        self._fade.stop()
        self._fade.setStartValue(0.0)
        self._fade.setEndValue(1.0)
        self._fade.start()

    def _copy_translation(self) -> None:
        if not self._translated_text:
            return
        QApplication.clipboard().setText(self._translated_text)
        old_text = self.copy_button.text()
        self.copy_button.setText(_tr("Copied"))
        QTimer.singleShot(900, lambda: self.copy_button.setText(old_text))

    def _open_full(self) -> None:
        self._manual_debounce.stop()
        # Always read the box: the user may have edited the text since it loaded.
        self._source_text = self.source_edit.toPlainText().strip()
        self.open_full_requested.emit(
            self._source_text, self._translated_text, self._error_text
        )

    def _hide_popup(self) -> None:
        self._manual_debounce.stop()
        self._loading_timer.stop()
        self.hide()

    def paintEvent(self, event) -> None:
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        rect = QRectF(self.rect()).adjusted(0.5, 0.5, -0.5, -0.5)
        painter.setPen(QPen(self._palette.fill_hover_color, 1))
        painter.setBrush(QColor(self._palette.window))
        painter.drawRoundedRect(rect, 12, 12)

    def mousePressEvent(self, event) -> None:
        if event.button() == Qt.MouseButton.LeftButton and event.position().y() <= 42:
            self._drag_offset = event.globalPosition().toPoint() - self.frameGeometry().topLeft()
            event.accept()
            return
        super().mousePressEvent(event)

    def mouseMoveEvent(self, event) -> None:
        if self._drag_offset is not None and event.buttons() & Qt.MouseButton.LeftButton:
            self.move(event.globalPosition().toPoint() - self._drag_offset)
            event.accept()
            return
        super().mouseMoveEvent(event)

    def mouseReleaseEvent(self, event) -> None:
        self._drag_offset = None
        super().mouseReleaseEvent(event)
