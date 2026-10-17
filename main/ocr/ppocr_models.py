# -*- coding: utf-8 -*-
"""PP-OCR 模型的存放位置、内置清单、添加、删除和下载。

模型的判断和文件操作都在 ppocr_rust.models_*（两套实现共用），这里只定位目录、转换 JSON；
下载交给更新器进程，下完再由 ppocr_rust 把文件放到位。
"""
import json
import os
import sys
import tempfile
from pathlib import Path

from PySide6.QtCore import QObject, Signal

from core.logger import T, log_warning
from core.updater_process import UpdaterProcess


def models_root() -> Path:
    """下载和添加的模型放在这里：打包后是 exe 旁的 models/，源码运行时是仓库根目录的 models/。"""
    if getattr(sys, "frozen", False):
        return Path(sys.executable).resolve().parent / "models"
    return Path(__file__).resolve().parents[2] / "models"


def _roots() -> list:
    roots = [models_root()]
    meipass = getattr(sys, "_MEIPASS", None)
    if meipass:
        roots.append(Path(meipass) / "models")
    return roots


def _ppocr():
    import ppocr_rust
    return ppocr_rust


def catalog() -> dict:
    return json.loads(_ppocr().models_catalog())


def catalog_entry(catalog_id: str):
    return next((m for m in catalog()["models"] if m["id"] == catalog_id), None)


def installed() -> list:
    """已安装的模型；同一个 id 在多个目录里都有时只取第一个。"""
    found, seen = [], set()
    for root in _roots():
        for model in json.loads(_ppocr().models_discover(str(root))):
            if model["id"] not in seen:
                seen.add(model["id"])
                found.append(model)
    return found


def resolve(model_id: str):
    """按 id 找已安装的模型，没有时返回 None。"""
    for root in _roots():
        text = _ppocr().models_resolve(str(root), model_id)
        if text:
            return json.loads(text)
    return None


def auto_choice():
    """「自动」用的已装模型：按清单里的优先级挑，手动添加的排最后；一个都没装时返回 None。"""
    for root in _roots():
        text = _ppocr().models_auto_choice(str(root))
        if text:
            return json.loads(text)
    return None


def import_model(name: str, det: str, rec: str, dict_path=None) -> dict:
    """复制并校验用户选的模型文件；校验失败抛 ppocr_rust.OcrError，不留下半成品。"""
    return json.loads(_ppocr().models_import(str(models_root()), name, det, rec, dict_path))


def remove(model_id: str):
    _ppocr().models_remove(str(models_root()), model_id)


def clean_staging():
    root = models_root()
    if root.is_dir():
        _ppocr().models_clean_staging(str(root))


def _install_exe() -> str:
    """更新器把模型下到这个路径同目录下的 models/。"""
    if getattr(sys, "frozen", False):
        return sys.executable
    return str(models_root().parent / "jietuba.exe")


class ModelUpdaterProcess(UpdaterProcess):
    def error_text(self, payload) -> str:
        code = payload.get("code")
        log_warning(T("模型下载报错 {code}: {message}", code=code, message=payload.get("message")), "OCR")
        if code in ("network", "source"):
            return self.tr("Could not download the model. Check your network connection and try again.")
        if code in ("hash", "size"):
            return self.tr("The downloaded model file is damaged. Try again.")
        if code == "permission":
            return self.tr("No permission to write to the models folder.")
        if code in ("cancelled", "timeout"):
            return self.tr("The download was cancelled.")
        return self.tr("The model download stopped with an error (%1).").replace("%1", str(code or "unknown"))


class ModelDownload(QObject):
    """下载一个清单模型。progress 报全部文件合计的已下载/总字节数，finished 带就位后的模型；
    不论成败，更新器进程退出后发 stopped。"""

    progress = Signal(object, object)
    finished = Signal(object)
    failed = Signal(str)
    stopped = Signal()

    def __init__(self, parent=None):
        super().__init__(parent)
        self._runner = ModelUpdaterProcess(self)
        self._runner.event_received.connect(self._event)
        self._runner.failed.connect(self._fail)
        self._runner.finished.connect(self._done)
        self._request = None
        self._catalog_id = ""
        self._error = ""

    @property
    def busy(self) -> bool:
        return self._runner.busy

    @property
    def catalog_id(self) -> str:
        return self._catalog_id

    def start(self, catalog_id: str) -> bool:
        entry = catalog_entry(catalog_id)
        if entry is None or self.busy:
            return False
        fd, path = tempfile.mkstemp(prefix="jietuba-model-", suffix=".json")
        with os.fdopen(fd, "w", encoding="utf-8") as fp:
            json.dump({"staging": catalog_id, "files": entry["files"]}, fp)
        self._request, self._catalog_id, self._error = path, catalog_id, ""
        arguments = ["--install-exe", _install_exe(), "--model-file", path]
        if not self._runner.start("download-model", arguments, timeout_ms=3_600_000):
            self._remove_request()
            return False
        return True

    def cancel(self):
        self._runner.cancel()

    def _event(self, event, _transaction, data):
        if event == "progress":
            self.progress.emit(int(data.get("received", 0)), int(data.get("total", 0)))
        elif event == "downloaded":
            try:
                model = json.loads(_ppocr().models_finish_download(str(models_root()), self._catalog_id))
            except Exception as exc:
                self._fail(str(exc))
                return
            self.finished.emit(model)

    def _fail(self, reason):
        if not self._error:
            self._error = reason
            self.failed.emit(reason)

    def _done(self):
        self._remove_request()
        self.stopped.emit()

    def _remove_request(self):
        if self._request:
            try:
                os.remove(self._request)
            except OSError:
                pass
            self._request = None
