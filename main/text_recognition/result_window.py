"""文字识别结果窗口：识别出的文字放在一块可改的文本里，配一个复制

识别在后台线程里跑，窗口先出来显示"识别中…"：截图界面这时已经关了，让用户对着空屏
等不如先把窗口摆出来。文本框出结果后可编辑——识别难免有错字，就地改完再复制，比复制
出去再改顺手。

窗口关掉只是把结果丢掉，线程照常跑完（识别打不断），见 recognizer 模块。
"""

from PySide6.QtCore import Qt, QTimer
from PySide6.QtGui import QCursor
from PySide6.QtWidgets import QApplication, QHBoxLayout, QVBoxLayout

from core import safe_event
from core.i18n import make_tr
from core.logger import log_info, T
from core.ui_scale import configure_dialog_control, dialog_scaled, scale_dialog_font
from core.ui_theme import get_ui_theme
from ui.dialogs import track_modeless_dialog
from ui.fluent_lite import (
    FONT_FAMILY, CaptionLabel, FluentTitleBar, FrostedFramelessDialog,
    PrimaryPushButton, TextEdit, scrollbar_qss, ui_tokens,
)

from .recognizer import NO_TEXT, UNAVAILABLE, recognize_async

_tr = make_tr("TextRecognitionWindow")


def show_text_recognition(image):
    """识别 image 里的文字并弹出结果窗口。截图工具栏的「文字识别」按钮走这里"""
    window = TextRecognitionWindow(image)
    track_modeless_dialog(window)
    window.show()
    window.raise_()
    window.activateWindow()
    return window


def _reason_text(reason):
    """失败原因键 → 给人看的话。每次现取，语言切换后再开窗口拿到的就是新译文"""
    return {
        UNAVAILABLE: _tr("This build does not include the text recognition engine."),
        NO_TEXT: _tr("No text found in the selection."),
    }.get(reason, _tr("Text recognition failed."))


class TextRecognitionWindow(FrostedFramelessDialog):
    """image 是选区底图；识别在构造时就开始，结果回来再填进文本框"""

    WIDTH = 520
    HEIGHT = 420

    def __init__(self, image, parent=None):
        super().__init__(parent)
        scale_dialog_font(self)
        self.setAttribute(Qt.WidgetAttribute.WA_DeleteOnClose, True)
        title_bar = FluentTitleBar(self)
        configure_dialog_control(title_bar)
        self.setTitleBar(title_bar)
        title_bar.iconLabel.hide()
        # 同扫码结果窗口：隐藏图标后标题会紧贴窗口左边，补回原生标题栏的留白
        title_bar.hBoxLayout.setContentsMargins(dialog_scaled(12), 0, 0, 0)
        self.setWindowTitle(_tr("Text recognition"))

        self.status_label = CaptionLabel(_tr("Recognizing..."), self)
        configure_dialog_control(self.status_label)
        self.text_edit = TextEdit(self)
        self.text_edit.setAcceptRichText(False)
        self.text_edit.setReadOnly(True)    # 识别中不给改，出结果再放开

        self.copy_button = PrimaryPushButton(_tr("Copy"), self)
        configure_dialog_control(self.copy_button)
        self.copy_button.setEnabled(False)
        self.copy_button.clicked.connect(self._copy)

        footer = QHBoxLayout()
        footer.setSpacing(dialog_scaled(8))
        footer.addWidget(self.status_label, 1)
        footer.addWidget(self.copy_button)

        root = QVBoxLayout(self)
        root.setContentsMargins(
            dialog_scaled(12), title_bar.height() + dialog_scaled(4),
            dialog_scaled(12), dialog_scaled(12),
        )
        root.setSpacing(dialog_scaled(8))
        root.addWidget(self.text_edit, 1)
        root.addLayout(footer)

        self._apply_theme()
        get_ui_theme().theme_changed.connect(self._apply_theme)
        self._place()

        self._thread = recognize_async(image, self._on_recognized)
        # 线程一结束就把引用放掉：它跑完就被回收，而取消掉的那次不会有结果回来，只有
        # finished 一定到。两个槽都是本窗口的方法，窗口先销毁时 Qt 会自己断开。
        self._thread.finished.connect(self._forget_thread)

    def _apply_theme(self, _tokens=None):
        t = ui_tokens(self)
        self.text_edit.setStyleSheet(
            f"QTextEdit {{ background: {t.input_background}; color: {t.text}; "
            f"border: 1px solid {t.border}; border-radius: 6px; padding: {dialog_scaled(8)}px {dialog_scaled(10)}px; "
            f"font: {dialog_scaled(14)}px {FONT_FAMILY}; selection-background-color: {t.accent}; "
            f"selection-color: {t.selected_text}; }}"
            + scrollbar_qss(self)
        )

    def _place(self):
        """摆在鼠标所在屏幕的正中；屏幕小就跟着缩"""
        screen = QApplication.screenAt(QCursor.pos()) or QApplication.primaryScreen()
        available = screen.availableGeometry()
        self.resize(
            min(dialog_scaled(self.WIDTH), available.width()),
            min(dialog_scaled(self.HEIGHT), available.height()),
        )
        self.move(available.center() - self.rect().center())

    def _forget_thread(self):
        self._thread = None

    def _on_recognized(self, text, reason):
        if reason:
            self.status_label.setText(_reason_text(reason))
            return

        log_info(T("文字识别完成: {count} 个字符", count=len(text)), "OCR")
        self.text_edit.setReadOnly(False)
        self.text_edit.setPlainText(text)
        self.status_label.setText(_tr("Recognized %1 characters").replace("%1", str(len(text))))
        self.copy_button.setEnabled(True)
        self.text_edit.setFocus()

    def _copy(self):
        QApplication.clipboard().setText(self.text_edit.toPlainText())
        self.copy_button.setText(_tr("Copied"))
        QTimer.singleShot(1500, self.copy_button, lambda: self.copy_button.setText(_tr("Copy")))

    @safe_event
    def closeEvent(self, event):
        if self._thread is not None:
            self._thread.cancel()   # 结果不要了；线程自己跑完、自己回收
            self._thread = None
        super().closeEvent(event)
