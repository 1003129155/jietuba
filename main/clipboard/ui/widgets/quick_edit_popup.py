# -*- coding: utf-8 -*-
"""文本条目的快速编辑浮层。"""

from dataclasses import dataclass
from typing import Optional

from PySide6.QtCore import QEvent, QPoint, QRect, Qt, QTimer, Signal
from PySide6.QtGui import QTextCursor
from PySide6.QtWidgets import QHBoxLayout, QLabel, QPushButton, QVBoxLayout, QWidget

from core import safe_event
from core.i18n import make_tr
from core.shortcut_manager import load_inapp_bindings, match_inapp_binding
from settings import get_tool_settings_manager
from ui.fluent_lite import TextEdit
from ui.key_chip import format_shortcut_text

from ...core import ClipboardItem
from ..theme.theme_styles import ThemeStyleGenerator
from ..theme.themes import Theme
from .preview_popup import side_position

_tr = make_tr("ClipboardQuickEdit")

SAVE_SHORTCUT = "inapp_clipboard_edit_save"
SAVE_AND_PASTE_SHORTCUT = "inapp_clipboard_edit_save_paste"


@dataclass(frozen=True)
class QuickEditResult:
    item_id: int
    text: Optional[str]  # None 表示取消
    paste: bool = False
    focus_left: bool = False  # 因焦点离开而结束，窗口不应再抢回焦点


class QuickEditPopup(QWidget):
    """只负责编辑与按键，保存和粘贴由剪贴板窗口处理。

    以剪贴板窗口为父的独立小窗：剪贴板窗口的失焦隐藏会把它认作自己的界面。
    按钮不接收焦点，焦点离开编辑框即视为点到了外面。
    """

    finished = Signal(object)  # QuickEditResult

    def __init__(self, parent: QWidget):
        super().__init__(
            parent,
            Qt.WindowType.Tool | Qt.WindowType.FramelessWindowHint | Qt.WindowType.WindowStaysOnTopHint,
        )
        self.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)
        self._item_id: Optional[int] = None
        self._bindings = {}

        layout = QVBoxLayout(self)
        layout.setContentsMargins(8, 8, 8, 8)
        layout.setSpacing(6)

        self.editor = TextEdit()
        self.editor.setAcceptRichText(False)
        self.editor.installEventFilter(self)
        layout.addWidget(self.editor)

        footer = QHBoxLayout()
        footer.setSpacing(6)
        self.format_hint = QLabel()
        footer.addWidget(self.format_hint, 1)
        self.save_button = QPushButton()
        self.save_and_paste_button = QPushButton()
        self.save_and_paste_button.setObjectName("primary")
        for button in (self.save_button, self.save_and_paste_button):
            button.setFocusPolicy(Qt.FocusPolicy.NoFocus)
            button.setCursor(Qt.CursorShape.PointingHandCursor)
            footer.addWidget(button)
        layout.addLayout(footer)

        self.save_button.clicked.connect(lambda: self.commit(paste=False))
        self.save_and_paste_button.clicked.connect(lambda: self.commit(paste=True))

        self.resize(420, 240)

    @property
    def is_open(self) -> bool:
        return self._item_id is not None

    def open_for(self, item: ClipboardItem, anchor: QPoint, avoid_rect: QRect, theme: Theme):
        self._item_id = item.id
        self.editor.setPlainText(item.content)
        self.editor.moveCursor(QTextCursor.MoveOperation.End)

        # 每次打开重读，设置里改过的键下次打开即生效
        self._bindings = load_inapp_bindings([SAVE_SHORTCUT, SAVE_AND_PASTE_SHORTCUT])
        config = get_tool_settings_manager()
        self.save_button.setText(_tr("Save"))
        self.save_button.setToolTip(format_shortcut_text(config.get_inapp_shortcut(SAVE_SHORTCUT)))
        self.save_and_paste_button.setText(_tr("Save and Paste"))
        self.save_and_paste_button.setToolTip(format_shortcut_text(config.get_inapp_shortcut(SAVE_AND_PASTE_SHORTCUT)))
        # 改了内容会清掉原始格式，粘贴只剩纯文本
        self.format_hint.setText(_tr("Formatting will be removed") if item.html_content else "")
        self.setStyleSheet(ThemeStyleGenerator(theme).generate_quick_edit_style())

        self.move(side_position(self.size(), anchor, avoid_rect, prefer_side="left"))
        self.show()
        self.raise_()
        self.activateWindow()
        self.editor.setFocus()

    def commit(self, paste: bool = False):
        self._finish(self.editor.toPlainText(), paste=paste)

    def cancel(self):
        self._finish(None)

    def close_for_focus_loss(self):
        self._finish(self.editor.toPlainText(), focus_left=True)

    def _finish(self, text: Optional[str], paste: bool = False, focus_left: bool = False):
        if self._item_id is None:
            return
        result = QuickEditResult(self._item_id, text, paste=paste, focus_left=focus_left)
        self._item_id = None
        self.hide()
        self.finished.emit(result)

    def _on_editor_focus_out(self):
        # 焦点回到编辑框（例如右键菜单关闭）就不算离开
        if self.is_open and not self.editor.hasFocus():
            self.close_for_focus_loss()

    @safe_event
    def eventFilter(self, obj, event):
        if obj is self.editor:
            if event.type() == QEvent.Type.KeyPress:
                if match_inapp_binding(event, SAVE_AND_PASTE_SHORTCUT, self._bindings):
                    self.commit(paste=True)
                    return True
                if match_inapp_binding(event, SAVE_SHORTCUT, self._bindings):
                    self.commit()
                    return True
                if event.key() == Qt.Key.Key_Escape:
                    self.cancel()
                    return True
            elif event.type() == QEvent.Type.FocusOut and event.reason() != Qt.FocusReason.PopupFocusReason:
                QTimer.singleShot(0, self._on_editor_focus_out)
        return super().eventFilter(obj, event)


__all__ = ["QuickEditPopup", "QuickEditResult"]
