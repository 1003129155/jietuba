# -*- coding: utf-8 -*-
"""Compact translation popup with result and manual-input modes."""

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
    QTextEdit,
    QVBoxLayout,
    QWidget,
)

from core.i18n import make_tr
from .translation_dialog import DARK, LIGHT, Palette
from .languages import TRANSLATION_LANGUAGES


_tr = make_tr("TranslationDialog")


class TranslationPopup(QWidget):
    """Single compact window reused for selected text and manual input."""

    open_full_requested = Signal(str, str, str)
    manual_translate_requested = Signal(str)
    manual_input_changed = Signal()
    target_lang_changed = Signal(str)  # 目标语言变更信号

    WIDTH = 420
    MIN_HEIGHT = 176
    MAX_HEIGHT = 520

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
        self._mode = "result"
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
        header.setContentsMargins(2, 0, 0, 0)
        header.setSpacing(8)

        self.title_label = QLabel(_tr("Translator"), self)
        self.title_label.setObjectName("popupTitle")
        header.addWidget(self.title_label)

        self.backend_badge = QLabel("DeepL API", self)
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

        self.source_edit = self._make_text_view("popupSource")
        self.source_edit.setMaximumHeight(92)
        self.source_edit.textChanged.connect(self._on_source_changed)
        root.addWidget(self.source_edit)

        self.divider = QFrame(self)
        self.divider.setObjectName("popupDivider")
        self.divider.setFixedHeight(1)
        root.addWidget(self.divider)

        self.result_edit = self._make_text_view("popupResult")
        self.result_edit.setMaximumHeight(208)
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

    def _make_text_view(self, object_name: str) -> QTextEdit:
        view = QTextEdit(self)
        view.setObjectName(object_name)
        view.setReadOnly(True)
        view.setAcceptRichText(False)
        view.setFrameShape(QFrame.Shape.NoFrame)
        view.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        view.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAsNeeded)
        view.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
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
                    # 显示loading状态
                    self.result_edit.setProperty("error", False)
                    self.result_edit.setPlainText(_tr("Translating..."))
                    self._refresh_result_style()
                    self.copy_button.setEnabled(False)
                    self._loading_step = 0
                    self._loading_timer.start()
                    
                    if self._mode == "result":
                        # result模式：通过manager重新翻译
                        self.manual_translate_requested.emit(self._source_text)
                    elif self._mode == "input":
                        # input模式：触发本地翻译
                        self.manual_translate_requested.emit(self._source_text)

    def set_target_lang(self, lang_code: str) -> None:
        """设置目标语言（外部调用）"""
        if lang_code in TRANSLATION_LANGUAGES:
            self._target_lang = lang_code
            self.lang_button.setText(TRANSLATION_LANGUAGES[lang_code])

    def show_loading(self, source_text: str, position: QPoint | None = None) -> None:
        self._set_mode("result")
        self._source_text = source_text.strip()
        self._translated_text = ""
        self._error_text = ""
        self.source_edit.setPlainText(self._source_text)
        self.result_edit.setPlainText(_tr("Translating..."))
        self.result_edit.setProperty("error", False)
        self.copy_button.setEnabled(False)
        self._loading_step = 0
        self._loading_timer.start()
        self._fit_content()
        self._place_near(position or QCursor.pos())
        self._show_animated()

    def show_input(self, position: QPoint | None = None) -> None:
        """Switch the shared popup to focused manual-entry mode."""
        self._loading_timer.stop()
        self._manual_debounce.stop()
        self._set_mode("input")
        self._source_text = ""
        self._translated_text = ""
        self._error_text = ""
        self.source_edit.clear()
        self.source_edit.setPlaceholderText(_tr("Enter text to translate..."))
        self.result_edit.clear()
        self.result_edit.setProperty("error", False)
        self.divider.hide()
        self.result_edit.hide()
        self.copy_button.hide()
        self._fit_content()
        self._place_near(position or QCursor.pos())
        self._show_animated()
        self.activateWindow()
        QTimer.singleShot(0, self._focus_manual_input)

    def _focus_manual_input(self) -> None:
        if self._mode != "input" or not self.isVisible():
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
        self._backend_ready = ready
        self.backend_badge.setText("DeepL API" if ready else _tr("Engine not configured"))
        self.backend_badge.setProperty("ready", ready)
        self.backend_badge.style().unpolish(self.backend_badge)
        self.backend_badge.style().polish(self.backend_badge)

    def set_theme(self, theme_name: str) -> None:
        self._palette = LIGHT if theme_name == "light" else DARK
        p = self._palette
        self.setStyleSheet(
            f"""
            QWidget#translationPopup {{ background: transparent; color: {p.text}; }}
            QLabel#popupTitle {{ font-size: 14px; font-weight: 650; color: {p.text}; }}
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
                color: {p.text_2}; background: transparent; border: none;
                padding: 2px 3px; font-size: 13px;
            }}
            QTextEdit#popupSource[input="true"] {{
                color: {p.text}; background: {p.field};
                border: 1px solid {p.fill_hover}; border-radius: 9px;
                padding: 7px 9px; font-size: 14px;
            }}
            QTextEdit#popupSource[input="true"]:focus {{ border-color: {p.accent}; }}
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
                border: 1px solid transparent; border-radius: 9px; padding: 6px 10px;
                font-size: 12px; font-weight: 600;
            }}
            QPushButton#popupChip {{ color: {p.text_2}; background: transparent; }}
            QPushButton#popupChip:hover {{ color: {p.text}; background: {p.fill}; }}
            QPushButton#popupChip:disabled {{ color: {p.text_3}; background: transparent; }}
            QPushButton#popupPrimary {{
                color: {p.text}; background: {p.fill}; border-color: {p.fill_hover};
            }}
            QPushButton#popupPrimary:hover {{ background: {p.fill_hover}; }}
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

    def _set_mode(self, mode: str) -> None:
        self._mode = mode
        is_input = mode == "input"
        self.setAttribute(
            Qt.WidgetAttribute.WA_ShowWithoutActivating, not is_input
        )
        self.source_edit.blockSignals(True)
        self.source_edit.setReadOnly(not is_input)
        self.source_edit.setTextInteractionFlags(
            Qt.TextInteractionFlag.TextEditorInteraction
            if is_input
            else Qt.TextInteractionFlag.TextSelectableByMouse
        )
        self.source_edit.setMaximumHeight(120 if is_input else 92)
        self.source_edit.setProperty("input", is_input)
        self.source_edit.style().unpolish(self.source_edit)
        self.source_edit.style().polish(self.source_edit)
        self.source_edit.blockSignals(False)

    def _on_source_changed(self) -> None:
        if self._mode != "input":
            return
        self._source_text = self.source_edit.toPlainText().strip()
        self._translated_text = ""
        self._error_text = ""
        self.manual_input_changed.emit()
        self._manual_debounce.stop()
        if not self._source_text:
            self._loading_timer.stop()
            self.divider.hide()
            self.result_edit.hide()
            self.copy_button.hide()
            self._fit_content()
            return
        if not self._backend_ready:
            self.show_error(_tr("API key not configured"))
            return
        self._manual_debounce.start()
        self._fit_content()

    def _request_manual_translation(self) -> None:
        if self._mode != "input":
            return
        text = self.source_edit.toPlainText().strip()
        if not text:
            return
        self._source_text = text
        self._translated_text = ""
        self._error_text = ""
        self.divider.show()
        self.result_edit.show()
        self.copy_button.show()
        self.copy_button.setEnabled(False)
        self.result_edit.setProperty("error", False)
        self.result_edit.setPlainText(_tr("Translating..."))
        self._loading_step = 0
        self._loading_timer.start()
        self._refresh_result_style()
        self._fit_content()
        self.manual_translate_requested.emit(text)

    def _refresh_result_style(self) -> None:
        self.result_edit.style().unpolish(self.result_edit)
        self.result_edit.style().polish(self.result_edit)

    def _fit_text_view(self, view: QTextEdit, minimum: int, maximum: int) -> None:
        doc_height = int(view.document().size().height()) + 10
        view.setFixedHeight(max(minimum, min(doc_height, maximum)))

    def _fit_content(self) -> None:
        self._fit_text_view(
            self.source_edit, 44 if self._mode == "input" else 38,
            120 if self._mode == "input" else 92,
        )
        if self.result_edit.isVisible():
            self._fit_text_view(self.result_edit, 44, 208)
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
        if self._mode == "input":
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
