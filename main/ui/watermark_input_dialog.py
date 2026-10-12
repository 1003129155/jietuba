"""输入水印：多行文字，可在光标处插入时间；下方预览加上去是什么样。"""

from datetime import datetime

import shiboken6
from PySide6.QtCore import QPoint, Qt
from PySide6.QtGui import QAction, QKeySequence, QShortcut, QTextCursor
from PySide6.QtWidgets import (
    QApplication, QDialog, QHBoxLayout, QLabel, QMenu, QToolButton, QVBoxLayout,
)

from canvas.items.watermark_item import expand_text
from core.i18n import make_tr
from core.ui_scale import configure_dialog_controls, dialog_scaled, scale_dialog_font
from ui.dialogs import exec_dialog
from ui.fluent_lite import CaptionLabel, PrimaryPushButton, PushButton, TextEdit, ui_tokens
from ui.fluent_lite.theme import ACCENT, FONT_FAMILY, INPUT_RADIUS

_tr = make_tr("WatermarkInputDialog")

# (按钮上的短名, Qt 日期格式, 说明)。短名是格式代号，各语言一致；说明随界面语言
TIME_FORMATS = (
    ("ISO 24h", "yyyy-MM-dd HH:mm:ss", "Year-month-day, 24-hour (China, Japan, Korea, ISO)"),
    ("Slash 24h", "yyyy/MM/dd HH:mm:ss", "Slash-separated date, 24-hour"),
    ("EU 24h", "dd/MM/yyyy HH:mm:ss", "Day/month/year, 24-hour (most of Europe)"),
    ("US 24h", "MM/dd/yyyy HH:mm:ss", "Month/day/year, 24-hour (United States)"),
    ("US 12h", "MM/dd/yyyy hh:mm:ss AP", "Month/day/year, 12-hour (United States and Canada)"),
    ("Date", "yyyy-MM-dd", "Date only"),
    ("Time", "HH:mm:ss", "Time only, 24-hour"),
    ("Time 12h", "hh:mm:ss AP", "Time only, 12-hour"),
    ("Weekday", "ddd yyyy-MM-dd HH:mm:ss", "Date and time with the weekday"),
    ("Millis", "yyyy-MM-dd HH:mm:ss.zzz", "With milliseconds, for logs"),
    ("Compact", "yyyyMMddHHmmss", "Compact, for file names"),
)
DEFAULT_TIME_FORMAT = TIME_FORMATS[0][1]


def time_token(time_format: str) -> str:
    return f"${time_format}$"


def short_name(time_format: str) -> str:
    """格式的短名；用户自己在文字里写的格式不在列表里，就直接显示格式本身。"""
    return next((name for name, fmt, _ in TIME_FORMATS if fmt == time_format), time_format)


class WatermarkInputDialog(QDialog):
    """水印文字输入框。Ctrl+Enter 应用；Enter 换行，水印可以有多行。"""

    BASE_WIDTH = 420
    BASE_EDIT_HEIGHT = 110

    def __init__(self, template: str, time_format: str = DEFAULT_TIME_FORMAT, parent=None):
        super().__init__(parent)
        scale_dialog_font(self)
        self.setWindowTitle(_tr("Input Watermark"))
        # 盖在全屏截图窗口上面，和截图里弹出的取色框一样置顶
        self.setWindowFlags(
            Qt.WindowType.Dialog | Qt.WindowType.WindowTitleHint
            | Qt.WindowType.WindowCloseButtonHint | Qt.WindowType.WindowStaysOnTopHint
        )
        tokens = ui_tokens(self)
        self.setStyleSheet(f"QDialog {{ background: {tokens.window}; }}")
        self._time_format = time_format if any(fmt == time_format for _, fmt, _ in TIME_FORMATS) \
            else DEFAULT_TIME_FORMAT

        self.edit = TextEdit(self)
        self.edit.setAcceptRichText(False)
        self.edit.setPlaceholderText(_tr("Type the watermark. Use the time button to insert the capture time."))
        self.edit.setPlainText(template or "")
        self.edit.setFixedHeight(dialog_scaled(self.BASE_EDIT_HEIGHT))
        self.edit.setStyleSheet(
            f"QTextEdit {{ color: {tokens.text}; background: {tokens.input_background};"
            f" border: 1px solid {tokens.border}; border-radius: {dialog_scaled(INPUT_RADIUS)}px;"
            f" padding: {dialog_scaled(4)}px; font: {dialog_scaled(14)}px {FONT_FAMILY};"
            f" selection-background-color: {ACCENT}; }}"
            f"QTextEdit:focus {{ border-color: {ACCENT}; }}"
        )
        self.edit.textChanged.connect(self._refresh_preview)

        self.preview = CaptionLabel("", self)
        self.preview.setWordWrap(True)
        self.preview.setTextFormat(Qt.TextFormat.PlainText)

        time_label = QLabel(_tr("Time:"), self)
        self.time_button = QToolButton(self)
        self.time_button.setPopupMode(QToolButton.ToolButtonPopupMode.MenuButtonPopup)
        self.time_button.setToolButtonStyle(Qt.ToolButtonStyle.ToolButtonTextOnly)
        self.time_button.setCursor(Qt.CursorShape.PointingHandCursor)
        self.time_button.clicked.connect(self.insert_time)
        self.time_menu = QMenu(self.time_button)
        self.time_menu.setToolTipsVisible(True)
        self.time_menu.aboutToShow.connect(self._fill_time_menu)
        self.time_button.setMenu(self.time_menu)
        self.time_button.setStyleSheet(
            f"QToolButton {{ color: {tokens.text}; background: {tokens.input_background};"
            f" border: 1px solid {tokens.border}; border-radius: {dialog_scaled(INPUT_RADIUS)}px;"
            f" padding: {dialog_scaled(4)}px {dialog_scaled(26)}px {dialog_scaled(4)}px {dialog_scaled(10)}px;"
            f" min-height: {dialog_scaled(22)}px; }}"
            f"QToolButton:hover {{ border-color: {tokens.border_hover}; background: {tokens.surface_strong}; }}"
            f"QToolButton::menu-button {{ width: {dialog_scaled(20)}px; border: none; }}"
        )

        apply_button = PrimaryPushButton(_tr("Apply"), self)
        apply_button.setDefault(False)
        apply_button.setAutoDefault(False)
        apply_button.clicked.connect(self.accept)
        cancel_button = PushButton(_tr("Cancel"), self)
        cancel_button.setAutoDefault(False)
        cancel_button.clicked.connect(self.reject)
        for sequence in (QKeySequence("Ctrl+Return"), QKeySequence("Ctrl+Enter")):
            QShortcut(sequence, self, activated=self.accept)

        bottom = QHBoxLayout()
        bottom.setSpacing(dialog_scaled(8))
        bottom.addWidget(time_label)
        bottom.addWidget(self.time_button)
        bottom.addStretch(1)
        bottom.addWidget(apply_button)
        bottom.addWidget(cancel_button)

        root = QVBoxLayout(self)
        root.setContentsMargins(dialog_scaled(16), dialog_scaled(14), dialog_scaled(16), dialog_scaled(14))
        root.setSpacing(dialog_scaled(8))
        root.addWidget(self.edit)
        root.addWidget(self.preview)
        root.addLayout(bottom)

        configure_dialog_controls(self)
        self.setFixedWidth(dialog_scaled(self.BASE_WIDTH))
        self._sync_time_button()
        self._refresh_preview()
        self.edit.moveCursor(QTextCursor.MoveOperation.End)
        self.edit.setFocus()

    def template(self) -> str:
        return self.edit.toPlainText().strip()

    def time_format(self) -> str:
        return self._time_format

    def insert_time(self, time_format: str = None):
        """在光标处插入时间占位；传了格式就先换成这个格式，下次点按钮也用它。"""
        if time_format:
            self._time_format = time_format
            self._sync_time_button()
        self.edit.insertPlainText(time_token(self._time_format))
        self.edit.setFocus()

    def _sync_time_button(self):
        self.time_button.setText(short_name(self._time_format))
        description = next(desc for _, fmt, desc in TIME_FORMATS if fmt == self._time_format)
        self.time_button.setToolTip(_tr("Insert the capture time") + "\n" + _tr(description))

    def _fill_time_menu(self):
        """菜单现开现建：每项带上此刻的示例，一眼看出格式长什么样。"""
        self.time_menu.clear()
        now = datetime.now()
        for name, fmt, description in TIME_FORMATS:
            # 制表符后面的部分按快捷键那一列右对齐，示例排成整齐的一列
            action = QAction(f"{name}\t{expand_text(time_token(fmt), now)}", self.time_menu)
            action.setToolTip(_tr(description))
            action.setCheckable(True)
            action.setChecked(fmt == self._time_format)
            action.triggered.connect(lambda _checked=False, f=fmt: self.insert_time(f))
            self.time_menu.addAction(action)

    def _refresh_preview(self):
        text = expand_text(self.edit.toPlainText())
        self.preview.setText(_tr("Preview: %1").replace("%1", text) if text else "")
        self.preview.setVisible(bool(text))

    def place_near(self, anchor):
        """贴着 anchor 下方弹出，下方放不下就放上方，左边对齐。"""
        self.adjustSize()
        top_left = anchor.mapToGlobal(QPoint(0, 0))
        screen = QApplication.screenAt(top_left) or QApplication.primaryScreen()
        area = screen.availableGeometry()
        gap = dialog_scaled(6)
        frame = self.frameGeometry().size()
        y = top_left.y() + anchor.height() + gap
        if y + frame.height() > area.bottom():
            y = top_left.y() - frame.height() - gap
        x = max(area.left(), min(top_left.x(), area.right() - frame.width()))
        self.move(x, max(area.top(), y))


def edit_watermark_text(anchor, settings_manager):
    """弹出输入框编辑水印文字。应用返回新文字，取消返回 None；用过的时间格式记下来。"""
    dialog = WatermarkInputDialog(
        settings_manager.get_screenshot_watermark_text(),
        settings_manager.get_screenshot_watermark_time_format(),
    )
    dialog.place_near(anchor)
    accepted = exec_dialog(dialog, owner=anchor)
    if not shiboken6.isValid(dialog):
        return None
    settings_manager.set_screenshot_watermark_time_format(dialog.time_format())
    return dialog.template() if accepted else None
