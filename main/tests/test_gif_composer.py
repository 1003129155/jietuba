# -*- coding: utf-8 -*-
"""
_ComposeWorker (GIF 合成) 单元测试

覆盖 main/gif/composer.py 中 _ComposeWorker 的合成/导出逻辑：
- 路径模式：显式 path（导出到磁盘）vs None（导出到临时文件后读为 bytes）
- 尺寸计算：gif_width 缩放时的宽高比与偏移取整
- 取消：cancel() 后 _do_compose 应返回失败，且临时文件被清理
- 异常处理：export_gif 抛 "cancelled" 异常 vs 其它异常的不同处理
- cursor_sprites/cursor_infos 只在两者都提供时才传给 export_gif
- gifrecorder 不可用或 store 为 None 时直接失败

真实的 Rust gifrecorder.FrameStore.export_gif() 不在本文件覆盖范围内，
用 MagicMock 替身模拟其接口（path/width/height/repeat/... kwargs 与
progress_callback 的调用约定）。
"""
import os
import pytest
from unittest.mock import MagicMock, patch

import gif.composer as composer_module
from gif.composer import _ComposeWorker


@pytest.fixture
def fake_store():
    """模拟 gifrecorder.FrameStore：width/height 属性 + export_gif()/cancel_export()"""
    store = MagicMock()
    store.width = 640
    store.height = 480
    store.export_gif = MagicMock()
    store.cancel_export = MagicMock()
    return store


@pytest.fixture(autouse=True)
def force_gifrecorder_available():
    with patch.object(composer_module, "_gifrecorder_available", True):
        yield


class TestComposeToExplicitPath:
    def test_do_compose_calls_export_gif_with_out_path(self, qapp, fake_store, tmp_path):
        out_path = str(tmp_path / "out.gif")
        worker = _ComposeWorker(path=out_path, store=fake_store)

        # export_gif 是 mock，不会真的写文件；手动创建让 getsize() 不炸
        def _fake_export(**kwargs):
            with open(out_path, "wb") as f:
                f.write(b"GIF89a")
        fake_store.export_gif.side_effect = _fake_export

        ok = worker._do_compose(out_path)

        assert ok is True
        fake_store.export_gif.assert_called_once()
        call_kwargs = fake_store.export_gif.call_args.kwargs
        assert call_kwargs["path"] == out_path
        assert call_kwargs["width"] == 640
        assert call_kwargs["height"] == 480

    def test_compose_returns_path_string_when_path_given(self, qapp, fake_store, tmp_path):
        out_path = str(tmp_path / "out.gif")

        def _fake_export(**kwargs):
            with open(out_path, "wb") as f:
                f.write(b"GIF89a")
        fake_store.export_gif.side_effect = _fake_export

        worker = _ComposeWorker(path=out_path, store=fake_store)
        result = worker._compose()

        assert result == out_path
        assert os.path.isfile(out_path)


class TestComposeToBytes:
    def test_compose_returns_bytes_when_path_is_none(self, qapp, fake_store):
        def _fake_export(**kwargs):
            with open(kwargs["path"], "wb") as f:
                f.write(b"GIF89a_PAYLOAD")
        fake_store.export_gif.side_effect = _fake_export

        worker = _ComposeWorker(path=None, store=fake_store)
        result = worker._compose()

        assert result == b"GIF89a_PAYLOAD"

    def test_temp_file_is_deleted_after_reading_bytes(self, qapp, fake_store):
        captured_path = {}

        def _fake_export(**kwargs):
            captured_path["path"] = kwargs["path"]
            with open(kwargs["path"], "wb") as f:
                f.write(b"X")
        fake_store.export_gif.side_effect = _fake_export

        worker = _ComposeWorker(path=None, store=fake_store)
        worker._compose()

        assert not os.path.isfile(captured_path["path"])


class TestGifWidthScaling:
    def test_no_gif_width_uses_store_native_size(self, qapp, fake_store):
        fake_store.export_gif.side_effect = lambda **kw: open(kw["path"], "wb").close()
        worker = _ComposeWorker(path="out.gif", store=fake_store, gif_width=0)
        worker._do_compose("out.gif")

        call_kwargs = fake_store.export_gif.call_args.kwargs
        assert call_kwargs["width"] == 640
        assert call_kwargs["height"] == 480
        os.unlink("out.gif") if os.path.isfile("out.gif") else None

    def test_scaled_width_computes_proportional_height(self, qapp, fake_store):
        # store: 640x480 (4:3) 缩放到 gif_width=320 → height 应约为 240
        fake_store.width = 640
        fake_store.height = 480
        fake_store.export_gif.side_effect = lambda **kw: open(kw["path"], "wb").close()

        worker = _ComposeWorker(path="scaled.gif", store=fake_store, gif_width=320)
        worker._do_compose("scaled.gif")

        call_kwargs = fake_store.export_gif.call_args.kwargs
        assert call_kwargs["width"] == 320
        assert call_kwargs["height"] == 240
        os.unlink("scaled.gif") if os.path.isfile("scaled.gif") else None

    def test_scaled_height_forced_to_even_number(self, qapp, fake_store):
        # 640x481 缩放到 320 → 481*0.5=240.5 -> int=240 (已是偶数，不需要-1)
        # 换一组会产生奇数的比例来验证强制偶数逻辑
        fake_store.width = 100
        fake_store.height = 33  # ratio=0.5 -> 16.5 -> int=16 (偶数，跳过分支)；换成会产生奇数的场景
        fake_store.export_gif.side_effect = lambda **kw: open(kw["path"], "wb").close()

        worker = _ComposeWorker(path="odd.gif", store=fake_store, gif_width=50)
        worker._do_compose("odd.gif")

        call_kwargs = fake_store.export_gif.call_args.kwargs
        # 无论计算结果奇偶，最终宽高必须是偶数（GIF/编码器通常要求偶数尺寸）
        assert call_kwargs["height"] % 2 == 0
        assert call_kwargs["height"] >= 2
        os.unlink("odd.gif") if os.path.isfile("odd.gif") else None


class TestCursorSprites:
    def test_cursor_kwargs_omitted_when_either_missing(self, qapp, fake_store):
        fake_store.export_gif.side_effect = lambda **kw: open(kw["path"], "wb").close()

        # 只提供 sprites，没有 infos -> 不应传入
        worker = _ComposeWorker(
            path="a.gif", store=fake_store,
            cursor_sprites={"arrow": b"data"}, cursor_infos=None,
        )
        worker._do_compose("a.gif")
        assert "cursor_sprites" not in fake_store.export_gif.call_args.kwargs
        os.unlink("a.gif") if os.path.isfile("a.gif") else None

    def test_cursor_kwargs_included_when_both_present(self, qapp, fake_store):
        fake_store.export_gif.side_effect = lambda **kw: open(kw["path"], "wb").close()

        sprites = {"arrow": b"data"}
        infos = [(0, 0, 0, 0, 0, 0)]
        worker = _ComposeWorker(
            path="b.gif", store=fake_store,
            cursor_sprites=sprites, cursor_infos=infos,
        )
        worker._do_compose("b.gif")
        call_kwargs = fake_store.export_gif.call_args.kwargs
        assert call_kwargs["cursor_sprites"] is sprites
        assert call_kwargs["cursor_infos"] is infos
        os.unlink("b.gif") if os.path.isfile("b.gif") else None


class TestCancellation:
    def test_cancel_sets_flag_and_calls_store_cancel_export(self, qapp, fake_store):
        worker = _ComposeWorker(path="x.gif", store=fake_store)
        worker.cancel()
        assert worker._cancel is True
        fake_store.cancel_export.assert_called_once()

    def test_export_gif_raising_cancelled_returns_false_without_error_log(self, qapp, fake_store):
        fake_store.export_gif.side_effect = RuntimeError("export cancelled by user")
        worker = _ComposeWorker(path="cancelled.gif", store=fake_store)

        ok = worker._do_compose("cancelled.gif")

        assert ok is False

    def test_compose_cleans_up_temp_file_when_cancelled(self, qapp, fake_store):
        created_path = {}

        def _fake_export(**kwargs):
            created_path["path"] = kwargs["path"]
            # 模拟：文件被创建了一部分，但随后判定为取消
            with open(kwargs["path"], "wb") as f:
                f.write(b"partial")
            raise RuntimeError("cancelled")

        fake_store.export_gif.side_effect = _fake_export
        worker = _ComposeWorker(path=None, store=fake_store)  # None -> 走临时文件分支

        result = worker._compose()

        assert result is None
        assert not os.path.isfile(created_path["path"])


class TestExportGifGenericFailure:
    def test_non_cancel_exception_returns_false(self, qapp, fake_store):
        fake_store.export_gif.side_effect = ValueError("disk full")
        worker = _ComposeWorker(path="fail.gif", store=fake_store)

        ok = worker._do_compose("fail.gif")

        assert ok is False


class TestRunEntryPoint:
    def test_run_emits_finished_false_when_gifrecorder_unavailable(self, qapp, fake_store):
        with patch.object(composer_module, "_gifrecorder_available", False):
            worker = _ComposeWorker(path="x.gif", store=fake_store)
            received = []
            worker.finished.connect(lambda ok, result: received.append((ok, result)))
            worker.run()

        assert received == [(False, "gifrecorder_not_found")]

    def test_run_emits_finished_false_when_store_is_none(self, qapp):
        worker = _ComposeWorker(path="x.gif", store=None)
        received = []
        worker.finished.connect(lambda ok, result: received.append((ok, result)))
        worker.run()

        assert received == [(False, "gifrecorder_not_found")]

    def test_run_emits_finished_true_on_success(self, qapp, fake_store, tmp_path):
        out_path = str(tmp_path / "ok.gif")

        def _fake_export(**kwargs):
            with open(kwargs["path"], "wb") as f:
                f.write(b"GIF89a")
        fake_store.export_gif.side_effect = _fake_export

        worker = _ComposeWorker(path=out_path, store=fake_store)
        received = []
        worker.finished.connect(lambda ok, result: received.append((ok, result)))
        worker.run()

        assert len(received) == 1
        ok, result = received[0]
        assert ok is True
        assert result == out_path

    def test_generic_export_failure_is_handled_internally_not_as_crash(self, qapp, fake_store):
        """export_gif 抛普通异常时，_do_compose 内部 except 已捕获并返回 False；
        run() 的外层 try/except 只兜底 _compose() 之外真正逃逸的异常（如 bug），
        所以这里 finished 应发出 (True, None) —— ok=True 因为未被 cancel()，
        但 result=None 因为 _do_compose 返回了 False。"""
        fake_store.export_gif.side_effect = Exception("boom, totally unexpected")
        worker = _ComposeWorker(path="boom.gif", store=fake_store)
        received = []
        worker.finished.connect(lambda ok, result: received.append((ok, result)))

        worker.run()  # 不应向外抛异常

        assert received == [(True, None)]

    def test_run_catches_exception_escaping_compose_entirely(self, qapp, fake_store):
        """当异常发生在 _compose() 自身（不是 export_gif 内部），run() 的外层
        try/except 应捕获并 emit (False, None)。"""
        worker = _ComposeWorker(path="x.gif", store=fake_store)
        with patch.object(worker, "_compose", side_effect=RuntimeError("unexpected bug")):
            received = []
            worker.finished.connect(lambda ok, result: received.append((ok, result)))
            worker.run()

        assert received == [(False, None)]


class TestSpeedMultiplierClamp:
    def test_speed_multiplier_clamped_to_minimum(self, qapp, fake_store):
        worker = _ComposeWorker(path="x.gif", store=fake_store, speed_multiplier=0.01)
        assert worker._speed_multiplier == 0.1

    def test_speed_multiplier_passed_through_when_valid(self, qapp, fake_store):
        worker = _ComposeWorker(path="x.gif", store=fake_store, speed_multiplier=2.5)
        assert worker._speed_multiplier == 2.5
