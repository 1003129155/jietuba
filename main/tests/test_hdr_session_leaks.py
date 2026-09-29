# -*- coding: utf-8 -*-
"""
截图会话泄漏回归测试

HDR 截图会话只能在创建它的线程上释放；任何一条路径把它漏到别的线程，pyo3 会拒绝释放，
那块屏的 DXGI duplication 就一直被占着，这个进程里再也建不起会话，截图只能回落 mss。

两个判据：sys.unraisablehook 收不到跨线程释放；每条路径之后，另一条线程能新建会话。
需要真实桌面（DXGI Desktop Duplication），建不起会话的环境（如 CI）自动跳过。
"""
import gc
import sys
import threading
from concurrent.futures import ThreadPoolExecutor
from unittest.mock import patch

import pytest
from PySide6.QtCore import QRect

from capture import capture_service as cs


def _duplication_is_free():
    """在另一条线程上新建并关掉一个会话；建不起来说明有 duplication 没被释放。"""
    result = {}

    def probe():
        try:
            capture = cs.hdrcapture.Capture(timeout_ms=0)
            capture.close()
            result["ok"] = True
        except Exception:
            result["ok"] = False

    worker = threading.Thread(target=probe)
    worker.start()
    worker.join()
    gc.collect()
    return result["ok"]


@pytest.fixture
def dropped(monkeypatch):
    if cs.hdrcapture is None:
        pytest.skip("hdrcapture 未安装")
    cs._HdrSession.reset().result()
    if not _duplication_is_free():
        pytest.skip("建不起 DXGI 会话（没有真实桌面）")
    events = []
    monkeypatch.setattr(sys, "unraisablehook", events.append)
    yield events
    cs._HdrSession.reset().result()
    cs._HdrSession._lent = False
    gc.collect()


def _assert_released(events):
    assert _duplication_is_free(), "有 DXGI 会话没有释放"
    assert not events, f"会话在别的线程上被释放: {[str(e.exc_value) for e in events]}"


def test_every_capture_path_then_lend(qapp, dropped):
    cs.warm_up_hdr_session("hdr").result()
    cs.CaptureService("hdr").capture_all_screens()
    cs.CaptureService("hdr").capture_region(QRect(100, 100, 300, 200))
    cs.grab_region_hdr(QRect(100, 100, 300, 200))
    cs.lend_hdr_session().result()
    _assert_released(dropped)


def test_error_handed_back_to_the_caller_does_not_pin_the_session(dropped):
    cs.warm_up_hdr_session("hdr").result()
    with pytest.raises(Exception) as caught:
        cs._HdrSession.call("grab", 99, timeout_ms=0)   # 非法显示器索引
    cs.lend_hdr_session().result()
    _assert_released(dropped)
    assert caught.value is not None                     # 异常对象一直活到这里


def test_refresh_release_and_engine_switch(dropped):
    cs.warm_up_hdr_session("hdr").result()
    cs.refresh_hdr_session("hdr").result()
    cs.refresh_hdr_session("hdr").result()
    cs._HdrSession.release()
    cs._HdrSession.drain()
    _assert_released(dropped)

    cs.warm_up_hdr_session("hdr").result()
    cs.apply_capture_engine("mss")
    cs._HdrSession.drain()
    _assert_released(dropped)


@pytest.mark.parametrize("record", [True, False])
def test_gif_window_hands_the_session_back(qapp, qtbot, dropped, record):
    import gif.frame_recorder as frame_recorder

    if not frame_recorder._gifrecorder_available or not hasattr(frame_recorder.gifrecorder.RecordSession, "prepare"):
        pytest.skip("gifrecorder 不支持预备录制")
    cs.warm_up_hdr_session("hdr").result()
    with patch.object(frame_recorder, "uses_hdr_engine", lambda engine=None: True):
        recorder = frame_recorder.FrameRecorder()
        recorder.set_fps(15)
        recorder.set_rect(QRect(100, 100, 322, 202))
        recorder.prepare()
        qtbot.wait(300)
        if record:
            recorder.start()
            qtbot.wait(300)
            with qtbot.waitSignal(recorder.stop_finished, timeout=5000):
                recorder.stop_async()
        recorder.release()
    cs._HdrSession.drain()
    assert cs._HdrSession._capture is not None, "关窗口后截图会话应已重建"
    cs.lend_hdr_session().result()
    _assert_released(dropped)


def test_shutdown_releases_the_session(dropped):
    # 用单独的线程池，退出不影响别的用例共用的那个
    original = cs._HdrSession._executor, cs._HdrSession._worker_id
    cs._HdrSession._executor = ThreadPoolExecutor(max_workers=1, thread_name_prefix="HdrCapture")
    try:
        cs.warm_up_hdr_session("hdr").result()
        cs.shutdown_hdr_session()
        _assert_released(dropped)
    finally:
        cs._HdrSession._executor, cs._HdrSession._worker_id = original
