# -*- coding: utf-8 -*-
"""PP-OCR 模型的添加、删除、下载就位，以及模型管理窗口。

用临时目录当 models/，真的调用 ppocr_rust 的模型接口；网络下载由更新器负责，这里只用
假的更新器进程模拟它的事件。
"""
import json
import shutil
from pathlib import Path

import pytest

ppocr_rust = pytest.importorskip("ppocr_rust")

from ocr import ppocr_models  # noqa: E402

REPO_MODELS = Path(__file__).resolve().parents[2] / "models"
DET, REC = REPO_MODELS / "PP-OCRv6_det_small.onnx", REPO_MODELS / "PP-OCRv6_rec_small.onnx"


@pytest.fixture
def root(monkeypatch, tmp_path):
    models = tmp_path / "models"
    models.mkdir()
    monkeypatch.setattr(ppocr_models, "models_root", lambda: models)
    monkeypatch.setattr(ppocr_models, "_roots", lambda: [models])
    return models


def _install_default(root):
    for path in (DET, REC):
        shutil.copy(path, root / path.name)


class TestStore:

    def test_import_lists_and_removes_a_local_model(self, root):
        assert ppocr_models.installed() == []
        model = ppocr_models.import_model("我的模型", str(DET), str(REC))
        assert model["id"] == "我的模型" and model["catalog_id"] is None
        assert [m["id"] for m in ppocr_models.installed()] == ["我的模型"]
        assert ppocr_models.resolve("我的模型")["rec"] == str(root / "我的模型" / REC.name)
        ppocr_models.remove("我的模型")
        assert ppocr_models.installed() == [] and ppocr_models.resolve("我的模型") is None

    def test_invalid_model_is_rejected_without_leftovers(self, root, tmp_path):
        broken = tmp_path / "broken.onnx"
        broken.write_bytes(b"not a model")
        with pytest.raises(ppocr_rust.OcrError):
            ppocr_models.import_model("broken", str(broken), str(REC))
        assert list(root.iterdir()) == []

    def test_default_model_can_be_removed_and_auto_moves_on(self, root):
        _install_default(root)
        assert ppocr_models.resolve("")["catalog_id"] == "ppocr-v6-small"
        assert ppocr_models.auto_choice()["id"] == ""
        ppocr_models.remove("")
        assert ppocr_models.resolve("") is None and ppocr_models.auto_choice() is None

    def test_catalog_marks_one_default(self):
        models = ppocr_models.catalog()["models"]
        assert [m["id"] for m in models if m.get("default")] == ["ppocr-v6-small"]
        assert ppocr_models.catalog_entry("ppocr-v5-korean")["languages"] == ["ko", "en"]
        assert ppocr_models.catalog_entry("missing") is None


class _FakeRunner:
    """冒充更新器进程：记下启动参数，测试手动发事件。"""

    def __init__(self, download):
        self.download = download
        self.busy = False
        self.started = None
        self.cancelled = False

    def start(self, command, arguments, timeout_ms=0):
        self.started = (command, arguments)
        self.busy = True
        return True

    def cancel(self):
        self.cancelled = True

    def finish(self):
        self.busy = False
        self.download._done()


@pytest.fixture
def download(root, qapp):
    job = ppocr_models.ModelDownload()
    job._runner = _FakeRunner(job)
    yield job
    job.deleteLater()


class TestDownload:

    def test_request_names_the_catalog_files(self, download):
        assert download.start("ppocr-v5-korean")
        command, arguments = download._runner.started
        assert command == "download-model"
        request = json.loads(Path(arguments[arguments.index("--model-file") + 1]).read_text(encoding="utf-8"))
        entry = ppocr_models.catalog_entry("ppocr-v5-korean")
        assert request == {"staging": "ppocr-v5-korean", "files": entry["files"]}
        assert Path(arguments[arguments.index("--install-exe") + 1]).parent / "models" == ppocr_models.models_root()
        download._runner.finish()
        assert not Path(arguments[arguments.index("--model-file") + 1]).exists()

    def test_downloaded_files_are_placed_and_reported(self, download, root):
        finished, stopped = [], []
        download.finished.connect(finished.append)
        download.stopped.connect(lambda: stopped.append(True))
        assert download.start("ppocr-v6-tiny")
        staging = root / ".download-ppocr-v6-tiny"
        staging.mkdir()
        for f in ppocr_models.catalog_entry("ppocr-v6-tiny")["files"]:
            (staging / f["name"]).write_bytes(b"\0" * f["size"])
        download._event("downloaded", "", {"staging": str(staging)})
        download._runner.finish()
        assert finished[0]["id"] == "ppocr-v6-tiny" and stopped == [True]
        assert ppocr_models.resolve("ppocr-v6-tiny")["catalog_id"] == "ppocr-v6-tiny"

    def test_unknown_model_does_not_start(self, download):
        assert download.start("missing") is False
        assert download._runner.started is None

    def test_error_codes_get_model_wording(self, qapp):
        runner = ppocr_models.ModelUpdaterProcess()
        assert "network" in runner.error_text({"code": "network"})
        assert "damaged" in runner.error_text({"code": "hash"})
        assert "cancelled" in runner.error_text({"code": "cancelled"})
        runner.deleteLater()


class TestDialog:

    @pytest.fixture
    def dialog(self, root, qapp, monkeypatch):
        from ui.settings_ui import ppocr_model_dialog as module
        _install_default(root)
        window = module.PpocrModelDialog()
        window._download._runner = _FakeRunner(window._download)
        yield window
        window._download._runner.busy = False
        window.deleteLater()
        qapp.processEvents()

    @staticmethod
    def _rows(window):
        from ui.settings_ui.ppocr_model_dialog import _ModelRow
        return [window._rows_layout.itemAt(i).widget() for i in range(window._rows_layout.count())
                if isinstance(window._rows_layout.itemAt(i).widget(), _ModelRow)]

    def test_rows_follow_catalog_and_installed_state(self, dialog):
        rows = self._rows(dialog)
        assert len(rows) == len(ppocr_models.catalog()["models"])
        default = next(r for r in rows if r.title.text() == "PP-OCRv6 small")
        assert default.button.text() == "Delete" and default.detail.text().endswith("Included")
        korean = next(r for r in rows if r.title.text() == "PP-OCRv5 Korean")
        assert korean.button.text() == "Download" and "Korean" in korean.detail.text()

    def test_download_progress_and_close_waits_for_cancel(self, dialog, qapp):
        korean = next(r for r in self._rows(dialog) if r.title.text() == "PP-OCRv5 Korean")
        korean.button.click()
        assert dialog._download._runner.started[0] == "download-model"
        rows = self._rows(dialog)
        row = next(r for r in rows if r.title.text() == "PP-OCRv5 Korean")
        assert row.progress is not None and row.button.text() == "Cancel" and row.button.isEnabled()
        assert all(not r.button.isEnabled() for r in rows if r.button is not None and r is not row)
        times = iter([0.0, 0.2, 2.0])
        dialog._clock = lambda: next(times)
        dialog._download.progress.emit(0, 20 * 1048576)
        dialog._download.progress.emit(1048576, 20 * 1048576)
        assert row.progress_label.text() == "1.0 MB / 20.0 MB"   # 不到半秒，还不报速度
        dialog._download.progress.emit(10 * 1048576, 20 * 1048576)
        assert row.progress.value() == 500
        assert row.progress_label.text() == "10.0 MB / 20.0 MB · 5.0 MB/s"
        row.button.click()
        assert dialog._download._runner.cancelled

        closed = []
        dialog.finished.connect(closed.append)
        dialog.reject()
        assert dialog._download._runner.cancelled and closed == []
        dialog._download._runner.finish()
        assert closed == [0]

    def test_refilling_keeps_the_scroll_position(self, dialog, qapp):
        dialog.resize(560, 300)
        dialog.show()
        qapp.processEvents()
        bar = dialog._scroll.verticalScrollBar()
        bar.setValue(bar.maximum() // 2)
        position = bar.value()
        assert position > 0
        dialog._fill()   # 点下载、删除后都会重建列表
        qapp.processEvents()
        assert bar.value() == position

    def test_local_model_rows_can_be_deleted(self, dialog, root, monkeypatch):
        ppocr_models.import_model("local", str(DET), str(REC))
        dialog._fill()
        changed = []
        dialog.models_changed.connect(lambda: changed.append(True))
        monkeypatch.setattr("ui.settings_ui.ppocr_model_dialog.show_confirm_dialog", lambda *a: True)
        local = next(r for r in self._rows(dialog) if r.title.text() == "local")
        assert local.detail.text() == "Added manually"
        local.button.click()
        assert not (root / "local").exists() and changed == [True]
        assert all(r.title.text() != "local" for r in self._rows(dialog))

    def test_import_result_reaches_the_dialog(self, dialog, monkeypatch):
        warnings = []
        monkeypatch.setattr("ui.settings_ui.ppocr_model_dialog.show_warning_dialog",
                            lambda _parent, title, message: warnings.append(message))
        dialog._importing = True
        dialog._on_imported(None, "字典对不上")
        assert warnings == ["字典对不上"] and dialog._importing is False
        dialog._on_imported({"name": "x"}, "")
        assert "x" in dialog._status.text()
