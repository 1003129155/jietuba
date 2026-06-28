# -*- coding: utf-8 -*-

"""文件内容表单构造器。

负责在文件型分组中构建文件路径、标题和浏览按钮等输入控件，
供新建和编辑 file 条目时复用。
"""

from PySide6.QtCore import Qt
from PySide6.QtWidgets import QFrame, QHBoxLayout, QLabel, QVBoxLayout

from ui.fluent_lite import BodyLabel, LineEdit, PushButton as FluentPushButton
from ui.fluent_lite.theme import ACCENT, ACCENT_SOFT
from ..layout_scale import scale_ui, scale_x, scale_y


class FileDropZone(QFrame):
    """文件拖拽输入区域。"""

    def __init__(self, on_path_dropped, prompt_text: str, parent=None):
        super().__init__(parent)
        self._on_path_dropped = on_path_dropped
        self._default_style = (
            "QFrame {"
            "border: 2px dashed #C8CCD3;"
            f"border-radius: {scale_ui(10)}px;"
            "background: #FCFCFD;"
            "}"
            "QLabel {"
            "color: #6B7280;"
            f"font-size: {scale_ui(13)}px;"
            f"padding: {scale_ui(8)}px;"
            "}"
        )
        self._hover_style = (
            "QFrame {"
            f"border: 2px dashed {ACCENT};"
            f"border-radius: {scale_ui(10)}px;"
            f"background: {ACCENT_SOFT};"
            "}"
            "QLabel {"
            f"color: {ACCENT};"
            f"font-size: {scale_ui(13)}px;"
            f"padding: {scale_ui(8)}px;"
            "font-weight: 500;"
            "}"
        )

        self.setAcceptDrops(True)
        self.setMinimumHeight(scale_y(204))

        layout = QVBoxLayout(self)
        layout.setContentsMargins(scale_x(12), scale_y(10), scale_x(12), scale_y(10))

        self.prompt_label = QLabel(prompt_text, self)
        self.prompt_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.prompt_label.setWordWrap(True)
        layout.addWidget(self.prompt_label)

        self._apply_default_style()

    def _apply_default_style(self):
        self.setStyleSheet(self._default_style)

    def _apply_hover_style(self):
        self.setStyleSheet(self._hover_style)

    @staticmethod
    def _extract_first_local_path(urls) -> str:
        for url in urls:
            local_path = url.toLocalFile()
            if local_path:
                return local_path
        return ""

    def _apply_dropped_urls(self, urls) -> bool:
        first_path = self._extract_first_local_path(urls)
        if not first_path:
            return False
        self._on_path_dropped(first_path)
        return True

    def dragEnterEvent(self, event):
        if event.mimeData().hasUrls() and self._extract_first_local_path(event.mimeData().urls()):
            event.acceptProposedAction()
            self._apply_hover_style()
            return
        event.ignore()

    def dragMoveEvent(self, event):
        if event.mimeData().hasUrls() and self._extract_first_local_path(event.mimeData().urls()):
            event.acceptProposedAction()
            return
        event.ignore()

    def dragLeaveEvent(self, event):
        self._apply_default_style()
        super().dragLeaveEvent(event)

    def dropEvent(self, event):
        self._apply_default_style()
        if self._apply_dropped_urls(event.mimeData().urls()):
            event.acceptProposedAction()
            return
        event.ignore()


def build_file_content_form(dialog, add_stretch: bool = True):
    """构建文件分组内容输入表单。"""
    dialog.selected_file_path = None

    title_label = BodyLabel(dialog.tr("Title (Optional)"))
    dialog.detail_layout.addWidget(title_label)

    dialog.title_input = LineEdit()
    dialog.title_input.setPlaceholderText(dialog.tr("Enter title..."))
    dialog.detail_layout.addWidget(dialog.title_input)

    file_label = BodyLabel(dialog.tr("File Path"))
    dialog.detail_layout.addWidget(file_label)

    dialog.file_path_input = LineEdit()
    dialog.file_path_input.setPlaceholderText(dialog.tr("Select a file..."))
    dialog.file_path_input.setReadOnly(True)
    dialog.detail_layout.addWidget(dialog.file_path_input)

    btn_row = QHBoxLayout()
    browse_file_btn = FluentPushButton(dialog.tr("Browse File"))
    browse_file_btn.clicked.connect(dialog._on_browse_file)
    btn_row.addWidget(browse_file_btn)

    browse_folder_btn = FluentPushButton(dialog.tr("Browse Folder"))
    browse_folder_btn.clicked.connect(dialog._on_browse_folder)
    btn_row.addWidget(browse_folder_btn)
    btn_row.addStretch()
    dialog.detail_layout.addLayout(btn_row)

    dialog.file_drop_zone = FileDropZone(
        dialog._set_selected_file_path,
        dialog.tr("Drag a file or folder here"),
        parent=dialog,
    )
    dialog.detail_layout.addWidget(dialog.file_drop_zone)

    if add_stretch:
        dialog.detail_layout.addStretch()


def build_edit_file_content_form(dialog, item, file_path: str):
    """构建文件条目的编辑表单。"""
    build_file_content_form(dialog, add_stretch=False)
    dialog.title_input.setText(item.title or "")
    dialog.selected_file_path = file_path or None
    dialog.file_path_input.setText(file_path)
