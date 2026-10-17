# -*- coding: utf-8 -*-
"""OCRManager 的引擎选择、切换和释放。

两个引擎都换成假的：这里测的是选哪个、什么时候加载、什么时候释放，
以及识别和释放互斥（截图工具 OCR 在识别中被释放会让进程直接崩掉）。
"""
import sys
import threading
import time

import pytest
from PySide6.QtGui import QImage

from ocr import ocr_manager as om
from ocr import ppocr_models
from ocr import snipping_tool_ocr


@pytest.mark.parametrize("engine, models, status", [
    (False, False, "missing_engine"),
    (True, False, "missing_models"),
    (True, True, "available"),
])
def test_ppocr_status_distinguishes_engine_and_models(monkeypatch, manager, engine, models, status):
    monkeypatch.setattr(om, "PP_RUST_ENGINE_AVAILABLE", engine)
    monkeypatch.setattr(ppocr_models, "resolve", lambda model_id: _MODELS.get(model_id) if models else None)
    monkeypatch.setattr(ppocr_models, "auto_choice", lambda: _MODELS[""] if models else None)
    assert om.get_ppocr_status() == status


class _FakeTextLine:
    def __init__(self, text):
        self.points = [(0.0, 0.0), (9.0, 0.0), (9.0, 5.0), (0.0, 5.0)]
        self.text = text
        self.score = 0.8


# 假装 models/ 里装着的模型：默认模型和一个韩文模型
_MODELS = {
    "": {"id": "", "det": "det.onnx", "rec": "rec.onnx", "dict": None},
    "korean": {"id": "korean", "det": "korean/det.onnx", "rec": "korean/rec.onnx", "dict": None},
}


class _FakePpEngine:
    def __init__(self, det_path, rec_path, dict_path=None):
        self.files = (det_path, rec_path, dict_path)
        self.closed = False

    def recognize(self, data, w, h, stride):
        return [_FakeTextLine("pp")]

    def close(self):
        self.closed = True


class _FakePpModule:
    def __init__(self):
        self.instances = []
        self.attempts = []
        self.broken = set()   # 这些 det 文件加载时报错，冒充损坏的模型

    def Engine(self, det_path, rec_path, dict_path=None):  # noqa: N802 —— 冒充扩展里的类名
        self.attempts.append(det_path)
        if det_path in self.broken:
            raise RuntimeError("Load model failed")
        engine = _FakePpEngine(det_path, rec_path, dict_path)
        self.instances.append(engine)
        return engine


class _FakeSnippingToolOcr:
    instances = []
    gate = None        # 设成 Event 时，识别会停在里面等它
    started = None

    def __init__(self, package):
        self.package = package
        self.closed = False
        _FakeSnippingToolOcr.instances.append(self)

    def recognize(self, image):
        if self.gate is not None:
            self.started.set()
            assert self.gate.wait(5)
        return [[[[0, 0], [9, 0], [9, 5], [0, 5]], "oneocr", 0.9]]

    def close(self):
        self.closed = True


@pytest.fixture
def engines(monkeypatch):
    pp = _FakePpModule()
    monkeypatch.setitem(sys.modules, "ppocr_rust", pp)
    monkeypatch.setattr(om, "PP_RUST_ENGINE_AVAILABLE", True)
    monkeypatch.setattr(ppocr_models, "resolve", _MODELS.get)
    monkeypatch.setattr(ppocr_models, "auto_choice", lambda: _MODELS[""])
    monkeypatch.setattr(om, "ONEOCR_AVAILABLE", True)
    monkeypatch.setattr(om, "_snipping_tool", ("Microsoft.ScreenSketch_1.0.0.0_x64__x", "src"))
    monkeypatch.setattr(snipping_tool_ocr, "SnippingToolOcr", _FakeSnippingToolOcr)
    monkeypatch.setattr(_FakeSnippingToolOcr, "instances", [])
    monkeypatch.setattr(_FakeSnippingToolOcr, "gate", None)
    return pp


@pytest.fixture
def manager(monkeypatch):
    monkeypatch.setattr(om.OCRManager, "_instance", None)
    monkeypatch.setattr(om.OCRManager, "_initialized", False)
    monkeypatch.setattr(om, "_configured_preference", lambda: "auto")
    manager = om.OCRManager()
    manager._pp_model_id = ""
    yield manager


def _image():
    image = QImage(60, 60, QImage.Format.Format_RGB888)
    image.fill(0)
    return image


def _text(result):
    return [item["text"] for item in result["data"]]


class TestResolve:

    def test_auto_prefers_the_snipping_tool(self, manager, engines):
        assert manager.set_engine("auto") is True
        assert manager.get_current_engine() == "oneocr"

    def test_auto_uses_ppocr_without_the_snipping_tool(self, manager, engines, monkeypatch):
        monkeypatch.setattr(om, "ONEOCR_AVAILABLE", False)
        assert manager.set_engine("auto") is True
        assert manager.get_current_engine() == "ppocr_rust"

    def test_lite_build_choosing_ppocr_reports_missing_engine(
            self, manager, engines, monkeypatch, qapp):
        monkeypatch.setattr(om, "PP_RUST_ENGINE_AVAILABLE", False)
        assert manager.set_engine("ppocr_rust") is False
        assert manager.get_current_engine() is None
        result = manager.recognize_pixmap(_image())
        assert result["code"] == -1
        assert "缺少引擎" in result["msg"]

    def test_missing_snipping_tool_falls_back_to_auto(self, manager, engines, monkeypatch, qapp):
        monkeypatch.setattr(om, "ONEOCR_AVAILABLE", False)
        assert manager.set_engine("oneocr") is True
        assert manager.get_current_engine() == "ppocr_rust"
        assert _text(manager.recognize_pixmap(_image())) == ["pp"]

    def test_unknown_preference_is_rejected(self, manager, engines):
        assert manager.set_engine("windows_media_ocr") is False
        assert manager.get_current_engine() is None

    def test_preference_comes_from_settings_once(self, manager, engines, monkeypatch, qapp):
        calls = []
        monkeypatch.setattr(om, "_configured_preference", lambda: calls.append(1) or "ppocr_rust")
        assert _text(manager.recognize_pixmap(_image())) == ["pp"]
        manager.recognize_pixmap(_image())
        assert calls == [1]


class TestSwitch:

    def test_engine_loads_on_first_use_not_on_switch(self, manager, engines, qapp):
        manager.set_engine("oneocr")
        assert _FakeSnippingToolOcr.instances == []
        assert _text(manager.recognize_pixmap(_image())) == ["oneocr"]
        assert len(_FakeSnippingToolOcr.instances) == 1

    def test_switch_releases_the_previous_engine_at_once(self, manager, engines, qapp):
        manager.set_engine("oneocr")
        manager.recognize_pixmap(_image())
        one = _FakeSnippingToolOcr.instances[0]

        manager.set_engine("ppocr_rust")
        assert one.closed is True
        assert manager._one_engine is None
        assert engines.instances == []

        assert _text(manager.recognize_pixmap(_image())) == ["pp"]
        manager.set_engine("oneocr")
        assert engines.instances[0].closed is True

    def test_switch_during_recognition_releases_after_it_finishes(self, manager, engines, qapp):
        manager.set_engine("oneocr")
        manager.initialize()
        one = _FakeSnippingToolOcr.instances[0]
        one.gate, one.started = threading.Event(), threading.Event()
        results = []
        worker = threading.Thread(target=lambda: results.append(manager.recognize_pixmap(_image())))
        worker.start()
        assert one.started.wait(5)

        start = time.perf_counter()
        manager.set_engine("ppocr_rust")
        # 设置页在 UI 线程里切换，不能等正在跑的识别
        assert time.perf_counter() - start < 0.5
        assert one.closed is False

        one.gate.set()
        worker.join(5)
        assert _text(results[0]) == ["oneocr"]
        assert one.closed is True

    def test_initialize_does_not_wait_for_running_recognition(self, manager, engines, qapp):
        """钉图时主线程会调 initialize；引擎已加载时不能卡在另一张图的识别上。"""
        manager.set_engine("oneocr")
        manager.initialize()
        one = _FakeSnippingToolOcr.instances[0]
        one.gate, one.started = threading.Event(), threading.Event()
        worker = threading.Thread(target=lambda: manager.recognize_pixmap(_image()))
        worker.start()
        assert one.started.wait(5)
        try:
            start = time.perf_counter()
            assert manager.initialize() is True
            assert time.perf_counter() - start < 0.5
        finally:
            one.gate.set()
            worker.join(5)

    def test_release_waits_for_running_recognition(self, manager, engines, qapp):
        manager.set_engine("oneocr")
        manager.initialize()
        one = _FakeSnippingToolOcr.instances[0]
        one.gate, one.started = threading.Event(), threading.Event()
        worker = threading.Thread(target=lambda: manager.recognize_pixmap(_image()))
        worker.start()
        assert one.started.wait(5)

        releaser = threading.Thread(target=manager.release_engine)
        releaser.start()
        releaser.join(0.3)
        assert releaser.is_alive()
        assert one.closed is False

        one.gate.set()
        worker.join(5)
        releaser.join(5)
        assert one.closed is True

    def test_released_engine_reloads_on_next_use(self, manager, engines, qapp):
        manager.set_engine("oneocr")
        manager.recognize_pixmap(_image())
        manager.release_engine()
        assert _text(manager.recognize_pixmap(_image())) == ["oneocr"]
        assert len(_FakeSnippingToolOcr.instances) == 2


class TestModelSwitch:

    def test_auto_uses_the_priority_choice_not_the_saved_model(self, manager, engines, monkeypatch, qapp):
        monkeypatch.setattr(om, "ONEOCR_AVAILABLE", False)
        monkeypatch.setattr(ppocr_models, "auto_choice", lambda: _MODELS["korean"])
        manager.set_ppocr_model("")
        assert manager.set_engine("auto") is True
        manager.recognize_pixmap(_image())
        assert engines.instances[0].files == ("korean/det.onnx", "korean/rec.onnx", None)

    def test_switching_model_reloads_on_next_use(self, manager, engines, monkeypatch, qapp):
        monkeypatch.setattr(om, "ONEOCR_AVAILABLE", False)
        manager.set_engine("ppocr_rust")
        assert _text(manager.recognize_pixmap(_image())) == ["pp"]
        first = engines.instances[0]
        manager.set_ppocr_model("korean")
        assert first.closed is False      # 切换时不动已加载的引擎，下次识别才换
        assert _text(manager.recognize_pixmap(_image())) == ["pp"]
        assert first.closed is True
        assert engines.instances[1].files == ("korean/det.onnx", "korean/rec.onnx", None)

    def test_missing_model_falls_back_to_auto(self, manager, engines, qapp):
        """选的模型被删了：按「自动」挑，有截图工具 OCR 就用它，设置不动。"""
        manager.set_ppocr_model("deleted")
        assert manager.set_engine("ppocr_rust") is True
        assert _text(manager.recognize_pixmap(_image())) == ["oneocr"]
        assert manager._pp_model_id == "deleted"

    def test_missing_model_without_snipping_tool_uses_the_auto_model(self, manager, engines, monkeypatch, qapp):
        monkeypatch.setattr(om, "ONEOCR_AVAILABLE", False)
        manager.set_ppocr_model("deleted")
        assert manager.set_engine("ppocr_rust") is True
        assert _text(manager.recognize_pixmap(_image())) == ["pp"]
        assert engines.instances[0].files == ("det.onnx", "rec.onnx", None)

    def test_model_deleted_after_choosing_it_falls_back_on_next_use(self, manager, engines, monkeypatch, qapp):
        monkeypatch.setattr(om, "ONEOCR_AVAILABLE", False)
        installed = dict(_MODELS)
        monkeypatch.setattr(ppocr_models, "resolve", installed.get)
        manager.set_ppocr_model("korean")
        manager.set_engine("ppocr_rust")
        manager.recognize_pixmap(_image())
        del installed["korean"]
        assert _text(manager.recognize_pixmap(_image())) == ["pp"]
        assert engines.instances[-1].files == ("det.onnx", "rec.onnx", None)

    def test_no_model_at_all_reports_no_model(self, manager, engines, monkeypatch, qapp):
        monkeypatch.setattr(om, "ONEOCR_AVAILABLE", False)
        monkeypatch.setattr(ppocr_models, "resolve", lambda model_id: None)
        monkeypatch.setattr(ppocr_models, "auto_choice", lambda: None)
        assert manager.set_engine("ppocr_rust") is False
        result = manager.recognize_pixmap(_image())
        assert (result["code"], result["reason"]) == (-1, "no_model")
        assert engines.attempts == []

    def test_broken_model_falls_back_and_is_not_loaded_again(self, manager, engines, monkeypatch, qapp):
        monkeypatch.setattr(om, "ONEOCR_AVAILABLE", False)
        engines.broken.add("korean/det.onnx")
        manager.set_ppocr_model("korean")
        manager.set_engine("ppocr_rust")
        assert _text(manager.recognize_pixmap(_image())) == ["pp"]
        assert _text(manager.recognize_pixmap(_image())) == ["pp"]
        assert engines.attempts == ["korean/det.onnx", "det.onnx"]

    def test_broken_model_with_nothing_else_reports_why(self, manager, engines, monkeypatch, qapp):
        monkeypatch.setattr(om, "ONEOCR_AVAILABLE", False)
        monkeypatch.setattr(ppocr_models, "auto_choice", lambda: _MODELS["korean"])
        engines.broken.add("korean/det.onnx")
        manager.set_ppocr_model("korean")
        manager.set_engine("ppocr_rust")
        result = manager.recognize_pixmap(_image())
        assert (result["code"], result["reason"]) == (-1, "model_broken")

    def test_replaced_model_files_are_tried_again(self, manager, engines, monkeypatch, qapp, tmp_path):
        """坏模型按文件大小和修改时间记：重新下载换掉文件后照常加载。"""
        monkeypatch.setattr(om, "ONEOCR_AVAILABLE", False)
        det, rec = tmp_path / "det.onnx", tmp_path / "rec.onnx"
        det.write_bytes(b"x")
        rec.write_bytes(b"x")
        model = {"id": "mine", "det": str(det), "rec": str(rec), "dict": None}
        monkeypatch.setattr(ppocr_models, "resolve", lambda model_id: model)
        monkeypatch.setattr(ppocr_models, "auto_choice", lambda: model)
        engines.broken.add(str(det))
        manager.set_ppocr_model("mine")
        manager.set_engine("ppocr_rust")
        assert manager.recognize_pixmap(_image())["reason"] == "model_broken"
        engines.broken.clear()
        det.write_bytes(b"fixed")
        assert _text(manager.recognize_pixmap(_image())) == ["pp"]
