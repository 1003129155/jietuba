# -*- coding: utf-8 -*-
"""PP-OCR 模型的管理窗口：下载内置清单里的模型、添加本地模型、删除。

模型文件的增删在这里立即生效（下载和复制都要时间，不宜等「应用」）；
用哪个模型仍在设置页的下拉框里选，随「应用」保存。
"""
import time
from collections import deque

from PySide6.QtCore import Qt, QTimer, QUrl, Signal
from PySide6.QtGui import QDesktopServices
from PySide6.QtWidgets import (
    QApplication, QDialog, QGridLayout, QHBoxLayout, QProgressBar, QScrollArea, QVBoxLayout, QWidget,
)

from core import background_tasks
from core.i18n import make_tr
from core.ui_scale import configure_dialog_control, configure_dialog_controls, dialog_scaled, scale_dialog_font
from ocr import ppocr_models
from ui.dialogs import exec_dialog, get_open_file_name, show_confirm_dialog, show_warning_dialog
from ui.fluent_lite import (
    BodyLabel, CaptionLabel, LineEdit, PrimaryPushButton, PushButton, TransparentPushButton, ui_tokens,
)

_tr = make_tr("PpocrModelDialog")

# 速度按最近这么多秒的下载量算：进度事件约 100 ms 一次，逐次算的话数字会一直跳。
SPEED_WINDOW = 3.0


def language_names(codes) -> str:
    names = {
        "zh": _tr("Chinese"), "ja": _tr("Japanese"), "en": _tr("English"), "ko": _tr("Korean"),
        "latin": _tr("Latin-script languages"), "cyrillic": _tr("Cyrillic"), "th": _tr("Thai"), "el": _tr("Greek"),
    }
    return _tr(", ").join(names.get(code, code) for code in codes)


def _megabytes(size: int) -> str:
    return _tr("%1 MB").replace("%1", f"{size / 1048576:.1f}")


class _ModelRow(QWidget):
    """一行：名称、语种/大小/状态，右侧一个操作按钮（下载或删除）。
    正在下载的那一行，按钮换成进度条、已下载量和速度，以及取消按钮。"""

    def __init__(self, title, detail, action_text, parent=None, downloading=False):
        super().__init__(parent)
        self.title = BodyLabel(title, self)
        self.detail = CaptionLabel(detail, self)
        self.detail.setWordWrap(True)
        self.progress = self.progress_label = None
        if downloading:
            action_text = _tr("Cancel")
            self.progress = QProgressBar(self)
            self.progress.setTextVisible(False)
            self.progress.setRange(0, 0)
            self.progress.setFixedWidth(dialog_scaled(180))
            self.progress_label = CaptionLabel("", self)
        self.button = PushButton(action_text, self) if action_text else None

        text = QVBoxLayout()
        text.setContentsMargins(0, 0, 0, 0)
        text.setSpacing(dialog_scaled(2))
        text.addWidget(self.title)
        text.addWidget(self.detail)

        layout = QHBoxLayout(self)
        layout.setContentsMargins(dialog_scaled(10), dialog_scaled(6), dialog_scaled(10), dialog_scaled(6))
        layout.setSpacing(dialog_scaled(10))
        layout.addLayout(text, 1)
        if self.progress is not None:
            progress = QVBoxLayout()
            progress.setContentsMargins(0, 0, 0, 0)
            progress.setSpacing(dialog_scaled(2))
            progress.addWidget(self.progress)
            progress.addWidget(self.progress_label)
            layout.addLayout(progress)
        if self.button is not None:
            layout.addWidget(self.button, 0, Qt.AlignmentFlag.AlignVCenter)

    def set_progress(self, received, total, speed):
        """speed 为每秒字节数，还算不出时为 None。"""
        self.progress.setRange(0, 1000)
        self.progress.setValue(int(received * 1000 / total))
        text = f"{_megabytes(received)} / {_megabytes(total)}"
        if speed is not None:
            text += " · " + _tr("%1 MB/s").replace("%1", f"{speed / 1048576:.1f}")
        self.progress_label.setText(text)


class PpocrModelDialog(QDialog):
    """模型有增删时发出 models_changed，设置页据此刷新下拉框；删掉模型时还发出 model_removed(模型 id)。"""

    models_changed = Signal()
    model_removed = Signal(str)
    _import_done = Signal(object, str)

    def __init__(self, parent=None):
        super().__init__(parent)
        scale_dialog_font(self)
        self.setWindowTitle(_tr("Manage PP-OCR Models"))
        self.setWindowFlags(
            Qt.WindowType.Dialog | Qt.WindowType.WindowTitleHint | Qt.WindowType.WindowCloseButtonHint
        )
        self.setStyleSheet(f"QDialog {{ background: {ui_tokens(self).window}; }}")
        self._close_after_download = False
        self._importing = False
        self._downloading_id = None
        self._download_row = None
        self._last_progress = None
        self._samples = deque()
        self._clock = time.monotonic

        self._download = ppocr_models.ModelDownload(self)
        self._download.progress.connect(self._on_progress)
        self._download.finished.connect(self._on_downloaded)
        self._download.failed.connect(self._on_download_failed)
        self._download.stopped.connect(self._on_download_stopped)
        self._import_done.connect(self._on_imported)

        self._rows = QWidget(self)
        self._rows_layout = QVBoxLayout(self._rows)
        self._rows_layout.setContentsMargins(0, 0, 0, 0)
        self._rows_layout.setSpacing(dialog_scaled(2))
        scroll = self._scroll = QScrollArea(self)
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QScrollArea.Shape.NoFrame)
        scroll.setWidget(self._rows)

        self._status = CaptionLabel("", self)
        self._status.setWordWrap(True)

        hint = CaptionLabel(
            _tr("Models are stored in the models folder next to the program. Recognition uses the model "
                "selected on the settings page."),
            self,
        )
        hint.setWordWrap(True)
        configure_dialog_control(hint)

        self._add_btn = PushButton(_tr("Add Local Model..."), self)
        self._add_btn.clicked.connect(self._add_local)
        folder_btn = TransparentPushButton(_tr("Open Models Folder"), self)
        folder_btn.clicked.connect(self._open_folder)
        close_btn = PrimaryPushButton(_tr("Close"), self)
        close_btn.clicked.connect(self.reject)
        buttons = QHBoxLayout()
        buttons.setSpacing(dialog_scaled(8))
        buttons.addWidget(self._add_btn)
        buttons.addWidget(folder_btn)
        buttons.addStretch(1)
        buttons.addWidget(close_btn)

        root = QVBoxLayout(self)
        root.setContentsMargins(dialog_scaled(16), dialog_scaled(16), dialog_scaled(16), dialog_scaled(14))
        root.setSpacing(dialog_scaled(10))
        root.addWidget(scroll, 1)
        root.addWidget(self._status)
        root.addWidget(hint)
        root.addLayout(buttons)

        configure_dialog_controls(self)
        self._fill()
        self._fit_height()

    def _fit_height(self):
        """行都放得下时不出滚动条；超过屏幕可用高度才让列表滚动。"""
        root = self.layout()
        margins = root.contentsMargins()
        others = margins.top() + margins.bottom() + root.spacing() * (root.count() - 1)
        for index in range(root.count()):
            item = root.itemAt(index)
            if item.widget() is not self._scroll:
                others += item.sizeHint().height()
        screen = self.screen() or QApplication.primaryScreen()
        limit = screen.availableGeometry().height() - dialog_scaled(80)
        # 行的建议高度比实际排出来的高，按最小高度算才不会在底下空出一截
        self.resize(dialog_scaled(560), min(others + self._rows_layout.minimumSize().height(), limit))

    # ── 列表 ──────────────────────────────────────────
    def _fill(self):
        """按清单和已装模型重建列表，滚动位置保持不变。"""
        scroll_bar = self._scroll.verticalScrollBar()
        position = scroll_bar.value()
        # 旧行移走后内容高度会先塌成 0，滚动条随之回到顶部；重建期间先按原高度撑住
        self._rows.setMinimumHeight(self._rows.height())
        while self._rows_layout.count():
            item = self._rows_layout.takeAt(0)
            if item.widget() is not None:
                # 先隐藏：删除要等事件循环，期间旧行会和新行叠在一起画出来
                item.widget().hide()
                item.widget().deleteLater()
        installed = ppocr_models.installed()
        by_catalog = {m["catalog_id"]: m for m in installed if m.get("catalog_id")}
        busy = self._download.busy or self._importing
        self._download_row = None
        for entry in ppocr_models.catalog()["models"]:
            model = by_catalog.get(entry["id"])
            size = sum(f["size"] for f in entry["files"])
            downloading = entry["id"] == self._downloading_id
            if downloading:
                state, action = _tr("Downloading..."), None
            elif model is None:
                state, action = _tr("Not downloaded"), _tr("Download")
            elif model["id"] == "":
                state, action = _tr("Included"), _tr("Delete")
            else:
                state, action = _tr("Downloaded"), _tr("Delete")
            row = _ModelRow(entry["name"], " · ".join((language_names(entry["languages"]), _megabytes(size), state)),
                            action, self._rows, downloading=downloading)
            if downloading:
                row.button.clicked.connect(self._download.cancel)
                self._download_row = row
                if self._last_progress is not None:
                    row.set_progress(*self._last_progress)
            elif row.button is not None:
                row.button.setEnabled(not busy)
                if model is None:
                    row.button.clicked.connect(lambda _=False, i=entry["id"]: self._start_download(i))
                else:
                    row.button.clicked.connect(lambda _=False, m=model: self._remove(m))
            self._rows_layout.addWidget(row)
        for model in installed:
            if model.get("catalog_id"):
                continue
            row = _ModelRow(model["name"], _tr("Added manually"), _tr("Delete"), self._rows)
            row.button.setEnabled(not busy)
            row.button.clicked.connect(lambda _=False, m=model: self._remove(m))
            self._rows_layout.addWidget(row)
        self._rows_layout.addStretch(1)
        configure_dialog_controls(self._rows)
        self._add_btn.setEnabled(not busy)
        # 新行是排队显示的，等它们显示后再放开高度、滚回原处
        def release():
            self._rows.setMinimumHeight(0)
            scroll_bar.setValue(position)

        QTimer.singleShot(0, release)

    # ── 下载 ──────────────────────────────────────────
    def _start_download(self, catalog_id):
        entry = ppocr_models.catalog_entry(catalog_id)
        if entry is None or not self._download.start(catalog_id):
            self._status.setText(_tr("Could not start the download."))
            return
        self._download_name = entry["name"]
        self._downloading_id = catalog_id
        self._last_progress = None
        self._samples.clear()
        self._status.setText("")
        self._fill()

    def _on_progress(self, received, total):
        if total <= 0:
            return
        now = self._clock()
        self._samples.append((now, received))
        while len(self._samples) > 2 and now - self._samples[0][0] > SPEED_WINDOW:
            self._samples.popleft()
        start, start_received = self._samples[0]
        speed = (received - start_received) / (now - start) if now - start >= 0.5 else None
        self._last_progress = (received, total, speed)
        if self._download_row is not None:
            self._download_row.set_progress(received, total, speed)

    def _on_downloaded(self, _model):
        self._status.setText(_tr("%1 is ready.").replace("%1", self._download_name))
        self.models_changed.emit()

    def _on_download_failed(self, reason):
        self._status.setText(reason)

    def _on_download_stopped(self):
        self._downloading_id = None
        self._last_progress = None
        self._fill()
        if self._close_after_download:
            super().reject()

    # ── 添加、删除 ────────────────────────────────────
    def _add_local(self):
        form = _ImportForm(self)
        if exec_dialog(form) != QDialog.DialogCode.Accepted:
            return
        name, det, rec, dictionary = form.values()
        self._importing = True
        self._status.setText(_tr("Checking %1...").replace("%1", name))
        self._fill()

        def work():
            try:
                self._import_done.emit(ppocr_models.import_model(name, det, rec, dictionary), "")
            except Exception as exc:
                self._import_done.emit(None, str(exc))

        background_tasks.start_thread(work, name="PP-OCR model import")

    def _on_imported(self, model, error):
        self._importing = False
        self._fill()
        if model is None:
            self._status.setText("")
            show_warning_dialog(self, _tr("Could not add the model"), error)
            return
        self._status.setText(_tr("%1 is ready.").replace("%1", model["name"]))
        self.models_changed.emit()

    def _remove(self, model):
        if not show_confirm_dialog(self, _tr("Delete Model"),
                                   _tr("Delete %1 and its files?").replace("%1", model["name"])):
            return
        try:
            ppocr_models.remove(model["id"])
        except Exception as exc:
            show_warning_dialog(self, _tr("Could not delete the model"), str(exc))
            return
        self._fill()
        self._status.setText("")
        self.model_removed.emit(model["id"])
        self.models_changed.emit()

    def show_status(self, text):
        self._status.setText(text)

    def _open_folder(self):
        root = ppocr_models.models_root()
        root.mkdir(parents=True, exist_ok=True)
        QDesktopServices.openUrl(QUrl.fromLocalFile(str(root)))

    def reject(self):
        """下载途中关窗口：先取消，等更新器退出、临时文件清掉后再关。"""
        if self._download.busy:
            self._close_after_download = True
            self._download.cancel()
            return
        if self._importing:
            return
        super().reject()


class _ImportForm(QDialog):
    """选检测模型、识别模型和可选的字典文件，并给模型起名。"""

    def __init__(self, parent=None):
        super().__init__(parent)
        scale_dialog_font(self)
        self.setWindowTitle(_tr("Add Local Model"))
        self.setWindowFlags(
            Qt.WindowType.Dialog | Qt.WindowType.WindowTitleHint | Qt.WindowType.WindowCloseButtonHint
        )
        self.setStyleSheet(f"QDialog {{ background: {ui_tokens(self).window}; }}")
        self._name = LineEdit(self)
        self._det = LineEdit(self)
        self._rec = LineEdit(self)
        self._dict = LineEdit(self)
        self._dict.setPlaceholderText(_tr("Optional: only if the recognition model has no built-in dictionary"))
        onnx = _tr("ONNX models (*.onnx)")
        grid = QGridLayout()
        grid.setHorizontalSpacing(dialog_scaled(8))
        grid.setVerticalSpacing(dialog_scaled(8))
        rows = (
            (_tr("Name"), self._name, None),
            (_tr("Detection model"), self._det, onnx),
            (_tr("Recognition model"), self._rec, onnx),
            (_tr("Dictionary"), self._dict, _tr("Text files (*.txt)")),
        )
        for index, (label, edit, file_filter) in enumerate(rows):
            grid.addWidget(BodyLabel(label, self), index, 0)
            grid.addWidget(edit, index, 1)
            if file_filter:
                browse = PushButton(_tr("Browse..."), self)
                browse.clicked.connect(lambda _=False, e=edit, f=file_filter: self._browse(e, f))
                grid.addWidget(browse, index, 2)
        self._ok = PrimaryPushButton(_tr("Add"), self)
        self._ok.clicked.connect(self.accept)
        cancel = PushButton(_tr("Cancel"), self)
        cancel.clicked.connect(self.reject)
        buttons = QHBoxLayout()
        buttons.addStretch(1)
        buttons.addWidget(self._ok)
        buttons.addWidget(cancel)
        root = QVBoxLayout(self)
        root.setContentsMargins(dialog_scaled(16), dialog_scaled(16), dialog_scaled(16), dialog_scaled(14))
        root.setSpacing(dialog_scaled(12))
        root.addLayout(grid)
        root.addLayout(buttons)
        for edit in (self._name, self._det, self._rec):
            edit.textChanged.connect(self._update_ok)
        configure_dialog_controls(self)
        self._update_ok()
        self.resize(dialog_scaled(560), self.sizeHint().height())

    def _browse(self, edit, file_filter):
        path, _ = get_open_file_name(self, _tr("Choose File"), edit.text(), file_filter)
        if path:
            edit.setText(path)
            if edit is self._rec and not self._name.text().strip():
                from pathlib import Path
                self._name.setText(Path(path).stem)

    def _update_ok(self):
        self._ok.setEnabled(all(e.text().strip() for e in (self._name, self._det, self._rec)))

    def values(self):
        dictionary = self._dict.text().strip() or None
        return self._name.text().strip(), self._det.text().strip(), self._rec.text().strip(), dictionary
