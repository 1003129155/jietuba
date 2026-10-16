"""文字识别结果窗口：识别出的文字放在一块可改的文本里，配一个复制

识别在后台线程里跑，窗口先出来显示"识别中…"：截图界面这时已经关了，让用户对着空屏
等不如先把窗口摆出来。文本框出结果后可编辑——识别难免有错字，就地改完再复制，比复制
出去再改顺手。

排版（见 layout 模块）是对识别出的行重排，每次都从原始的行重算，所以换来换去不会越改
越歪。重排走撤销栈：用户改过字之后再换排版，一次 Ctrl+Z 就能把改动拿回来。

窗口关掉只是把结果丢掉，线程照常跑完（识别打不断），见 recognizer 模块。
"""

from PySide6.QtCore import QPoint, QPointF, Qt, QTimer
from PySide6.QtGui import QColor, QCursor, QPainter, QPen, QTextCursor
from PySide6.QtWidgets import QApplication, QHBoxLayout, QMenu, QVBoxLayout

from core import safe_event
from core.i18n import make_tr
from core.logger import log_info, T
from core.platform_utils import set_window_topmost
from core.ui_scale import (
    configure_dialog_control, dialog_scaled, scale_dialog_font, widget_scaled,
)
from core.ui_theme import get_ui_theme
from settings import get_tool_settings_manager
from ui.dialogs import track_modeless_dialog
from ui.fluent_lite import (
    FONT_FAMILY, CaptionLabel, CheckBox, FluentTitleBar, FrostedFramelessDialog,
    PrimaryPushButton, TextEdit, TitleBarPinButton, TransparentPushButton,
    scrollbar_qss, ui_tokens,
)
from ui.fluent_lite.text_context_menu import style_popup_menu
from ui.submenu_position import avoid_submenu_overlap
from ui.toast import Toast

from . import layout as text_layout
from .placement import pick_screen, window_position
from .recognizer import MODEL_BROKEN, NO_MODEL, NO_TEXT, UNAVAILABLE, recognize_async

_tr = make_tr("TextRecognitionWindow")

# 排版菜单里的单选项，顺序就是菜单里的顺序。文案延后到打开菜单时才取：
# 这张表在导入时就建好了，直接存译文的话切换语言后菜单还是旧的
_LAYOUT_LABELS = (
    (text_layout.MODE_AUTO, lambda: _tr("Auto")),
    (text_layout.MODE_SINGLE_LINE, lambda: _tr("Remove line breaks")),
    (text_layout.MODE_MULTILINE, lambda: _tr("Keep every line")),
)
_PUNCTUATION_LABELS = (
    (text_layout.PUNCT_KEEP, lambda: _tr("As recognized")),
    (text_layout.PUNCT_HALFWIDTH, lambda: _tr("Half-width")),
    (text_layout.PUNCT_FULLWIDTH, lambda: _tr("Full-width")),
)


def show_text_recognition(image, anchor=None):
    """识别 image 里的文字并弹出结果窗口。截图工具栏的「文字识别」按钮走这里

    anchor 是识别区域在屏幕上的位置（全局坐标的 QRect），窗口摆放位置的设置要参照它。
    """
    window = TextRecognitionWindow(image, anchor=anchor)
    track_modeless_dialog(window)
    window.show()
    window.raise_()
    window.activateWindow()
    return window


def copy_text_recognition(image):
    """识别 image 里的文字直接复制，只在光标旁提示结果。快捷行为「文字识别直接复制」走这里"""
    return _RecognitionToast(image)


def _reason_text(reason):
    """失败原因键 → 给人看的话。每次现取，语言切换后再开窗口拿到的就是新译文"""
    return {
        UNAVAILABLE: _tr("This build does not include the text recognition engine."),
        NO_TEXT: _tr("No text found in the selection."),
        NO_MODEL: _tr("No PP-OCR model is installed. Download one in the OCR settings."),
        MODEL_BROKEN: _tr("The PP-OCR model could not be loaded. Download it again or choose another one in the OCR settings."),
    }.get(reason, _tr("Text recognition failed."))


def _render(lines):
    """按当前排版设置把识别出的行排成文本"""
    manager = get_tool_settings_manager()
    return text_layout.render(
        lines,
        mode=manager.get_ocr_layout_mode(),
        remove_spaces=manager.get_ocr_remove_spaces(),
        punctuation=manager.get_ocr_punctuation(),
    ).strip()


class _LayoutMenuButton(TransparentPushButton):
    """底栏的「排版」入口。右边画一个下拉箭头，表示点开是菜单而不是开关"""

    def _style_sheet(self):
        # 文字和箭头各占一边，右内边距要留得下箭头
        return super()._style_sheet() + (
            f"QPushButton {{ padding-right: {widget_scaled(self, 22)}px; }}"
        )

    def paintEvent(self, event):
        super().paintEvent(event)
        arm = widget_scaled(self, 4)
        x = self.width() - widget_scaled(self, 13)
        y = self.height() / 2.0
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        pen = QPen(QColor(ui_tokens(self).text_muted), max(1.2, widget_scaled(self, 1) * 1.2))
        pen.setCapStyle(Qt.PenCapStyle.RoundCap)
        pen.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
        painter.setPen(pen)
        painter.drawPolyline([
            QPointF(x - arm, y - arm / 2.0),
            QPointF(x, y + arm / 2.0),
            QPointF(x + arm, y - arm / 2.0),
        ])


class TextRecognitionWindow(FrostedFramelessDialog):
    """image 是选区底图；识别在构造时就开始，结果回来再填进文本框"""

    WIDTH = 520
    HEIGHT = 420

    def __init__(self, image, parent=None, anchor=None):
        super().__init__(parent)
        self._anchor = anchor
        self._lines = []
        scale_dialog_font(self)
        self.setAttribute(Qt.WidgetAttribute.WA_DeleteOnClose, True)
        title_bar = FluentTitleBar(self)
        configure_dialog_control(title_bar)
        self.setTitleBar(title_bar)
        title_bar.iconLabel.hide()
        # 同扫码结果窗口：隐藏图标后标题会紧贴窗口左边，补回原生标题栏的留白
        title_bar.hBoxLayout.setContentsMargins(dialog_scaled(12), 0, 0, 0)
        self.setWindowTitle(_tr("Text recognition"))

        self.pin_button = TitleBarPinButton(title_bar)
        configure_dialog_control(self.pin_button)
        self.pin_button.setChecked(get_tool_settings_manager().get_ocr_result_on_top())
        self.pin_button.clicked.connect(self._on_toggle_pin)
        title_bar.hBoxLayout.insertWidget(
            title_bar.hBoxLayout.indexOf(title_bar.minBtn), self.pin_button)

        self.status_label = CaptionLabel(_tr("Recognizing..."), self)
        configure_dialog_control(self.status_label)
        self.text_edit = TextEdit(self)
        self.text_edit.setAcceptRichText(False)
        self.text_edit.setReadOnly(True)    # 识别中不给改，出结果再放开

        self.layout_button = _LayoutMenuButton(_tr("Layout"), self)
        configure_dialog_control(self.layout_button)
        self.layout_button.setEnabled(False)
        self.layout_button.clicked.connect(self._show_layout_menu)

        self.copy_button = PrimaryPushButton(_tr("Copy"), self)
        configure_dialog_control(self.copy_button)
        self.copy_button.setEnabled(False)
        self.copy_button.clicked.connect(self._copy)

        # 勾上即生效：本窗口照常用完，下次识别起就不再弹窗；在设置的「快捷行为」页关回来
        self.copy_directly_check = CheckBox(_tr("Copy directly next time"), self)
        configure_dialog_control(self.copy_directly_check)
        self.copy_directly_check.setChecked(get_tool_settings_manager().get_ocr_copy_directly_enabled())
        self.copy_directly_check.toggled.connect(
            get_tool_settings_manager().set_ocr_copy_directly_enabled)

        footer = QHBoxLayout()
        footer.setSpacing(dialog_scaled(8))
        footer.addWidget(self.layout_button)
        footer.addWidget(self.status_label, 1)
        footer.addWidget(self.copy_directly_check)
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
        self._update_pin_tooltip()
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
        """按设置里的位置摆放；屏幕小就跟着缩"""
        manager = get_tool_settings_manager()
        position = manager.get_ocr_result_position()
        last = manager.get_ocr_result_last_center()
        last_center = QPoint(*last) if last is not None else None
        cursor = QCursor.pos()
        screen = pick_screen(
            position, self._anchor, cursor, last_center,
            QApplication.screens(), QApplication.primaryScreen(),
        )
        available = screen.availableGeometry()
        self.resize(
            min(dialog_scaled(self.WIDTH), available.width()),
            min(dialog_scaled(self.HEIGHT), available.height()),
        )
        self.move(window_position(position, self.size(), screen, self._anchor, cursor, last_center))

    # ------------------------------------------------------------------
    # 置顶
    # ------------------------------------------------------------------
    def showEvent(self, event):
        super().showEvent(event)
        # 只改窗口层级、不设 WindowStaysOnTopHint（原因见 set_window_topmost）。
        # 代价是 Qt 不会替我们维持，显示时按当前状态设一次
        set_window_topmost(int(self.winId()), self.pin_button.isChecked())

    def _on_toggle_pin(self, checked):
        get_tool_settings_manager().set_ocr_result_on_top(checked)
        self._update_pin_tooltip()
        set_window_topmost(int(self.winId()), checked)

    def _update_pin_tooltip(self):
        self.pin_button.setToolTip(
            _tr("Always on top: ON") if self.pin_button.isChecked()
            else _tr("Always on top: OFF"))

    # ------------------------------------------------------------------
    # 排版
    # ------------------------------------------------------------------
    def _show_layout_menu(self):
        menu = self._build_layout_menu()
        # 按钮贴着窗口底边，菜单只能往上展开
        menu.exec(self.layout_button.mapToGlobal(QPoint(0, -menu.sizeHint().height())))
        menu.deleteLater()

    def _build_layout_menu(self):
        """每次点开都按当前设置重建，勾选状态不用额外同步"""
        manager = get_tool_settings_manager()
        menu = QMenu(self)

        mode = manager.get_ocr_layout_mode()
        for value, label in _LAYOUT_LABELS:
            action = menu.addAction(label())
            action.setCheckable(True)
            action.setChecked(value == mode)
            action.triggered.connect(lambda _checked, chosen=value: self._set_layout_mode(chosen))

        menu.addSeparator()
        remove_spaces = menu.addAction(_tr("Remove spaces"))
        remove_spaces.setCheckable(True)
        remove_spaces.setChecked(manager.get_ocr_remove_spaces())
        remove_spaces.triggered.connect(self._set_remove_spaces)

        copy_on_close = menu.addAction(_tr("Copy text when closing"))
        copy_on_close.setCheckable(True)
        copy_on_close.setChecked(manager.get_ocr_copy_on_close())
        copy_on_close.triggered.connect(manager.set_ocr_copy_on_close)

        menu.addSeparator()
        punctuation_menu = menu.addMenu(_tr("Punctuation"))
        current = manager.get_ocr_punctuation()
        for value, label in _PUNCTUATION_LABELS:
            action = punctuation_menu.addAction(label())
            action.setCheckable(True)
            action.setChecked(value == current)
            action.triggered.connect(lambda _checked, chosen=value: self._set_punctuation(chosen))

        tokens = ui_tokens(self)
        style_popup_menu(menu, tokens)
        style_popup_menu(punctuation_menu, tokens)
        avoid_submenu_overlap(menu)
        return menu

    def _set_layout_mode(self, mode):
        get_tool_settings_manager().set_ocr_layout_mode(mode)
        self._relayout()

    def _set_remove_spaces(self, enabled):
        get_tool_settings_manager().set_ocr_remove_spaces(enabled)
        self._relayout()

    def _set_punctuation(self, punctuation):
        get_tool_settings_manager().set_ocr_punctuation(punctuation)
        self._relayout()

    def _relayout(self):
        self._show_result(_render(self._lines))

    def _show_result(self, text):
        """换掉文本框的内容并更新字数。

        走光标而不是 setPlainText：重排进得了撤销栈，用户改过的字一次 Ctrl+Z 就回来。
        """
        cursor = self.text_edit.textCursor()
        cursor.beginEditBlock()
        cursor.select(QTextCursor.SelectionType.Document)
        cursor.insertText(text)
        cursor.endEditBlock()
        self.status_label.setText(_tr("Recognized %1 characters").replace("%1", str(len(text))))

    # ------------------------------------------------------------------
    def _forget_thread(self):
        self._thread = None

    def _on_recognized(self, lines, reason):
        if reason:
            self.status_label.setText(_reason_text(reason))
            return

        self._lines = lines
        self.text_edit.setReadOnly(False)
        self._show_result(_render(lines))
        # 第一次填进去的不算改动，否则一次 Ctrl+Z 会把结果清空
        self.text_edit.document().clearUndoRedoStacks()
        log_info(T("文字识别完成: {count} 个字符", count=len(self.text_edit.toPlainText())), "OCR")
        self.copy_button.setEnabled(True)
        self.layout_button.setEnabled(True)
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
        manager = get_tool_settings_manager()
        text = self.text_edit.toPlainText()
        if manager.get_ocr_copy_on_close() and text:
            QApplication.clipboard().setText(text)
        if manager.get_ocr_result_position() == "last_position":
            center = self.geometry().center()
            manager.set_ocr_result_last_center(center.x(), center.y())
        super().closeEvent(event)


class _RecognitionToast(Toast):
    """识别期间显示"识别中…"，结果回来写进剪贴板并换成结果文案；失败时剪贴板不动"""

    def __init__(self, image):
        super().__init__(_tr("Recognizing..."))
        recognize_async(image, self._on_recognized)

    def _on_recognized(self, lines, reason):
        if reason:
            self.finish(_reason_text(reason))
            return
        text = _render(lines)
        QApplication.clipboard().setText(text)
        log_info(T("文字识别完成并直接复制: {count} 个字符", count=len(text)), "OCR")
        self.finish(_tr("Copied %1 characters").replace("%1", str(len(text))))
