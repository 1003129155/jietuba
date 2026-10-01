# -*- coding: utf-8 -*-
"""
FrameRecorder 单元测试

覆盖 main/gif/frame_recorder.py 中不依赖真实 Rust gifrecorder 扩展、
真实 Win32 截屏的纯逻辑部分：
- 录制状态机（start/pause/resume/stop/reset 的合法状态转换与非法转换的静默拒绝）
- 录制区域的 1px 内缩计算（避免录入选区边框线）
- _sync_frames_from_store 的时间戳同步与鼠标轨迹索引映射
- _tick 达到最大时长后自动停止
- 录制期间的全局滚轮记进鼠标轨迹（输入中心是测试替身，见 conftest）

真实截屏（RecordSession/FrameStore 的 Rust 实现）不在本文件覆盖范围内，用 mock 替身隔离。
"""
import pytest
from unittest.mock import MagicMock, patch

from PySide6.QtCore import QRect

import gif.frame_recorder as frame_recorder_module
from core.input_hub import input_hub
from gif.frame_recorder import FrameRecorder, RecordState, FrameData, CursorSnapshot


@pytest.fixture
def mock_gifrecorder():
    """伪造 gifrecorder 模块：FrameStore 和 RecordSession 均为可控 MagicMock"""
    fake_module = MagicMock()
    fake_module.STATE_RECORDING = "RECORDING"
    fake_module.STATE_PAUSED = "PAUSED"
    fake_module.STATE_STOPPED = "STOPPED"

    fake_store = MagicMock()
    fake_store.frame_count = 0
    fake_store.frame_timestamps = []
    fake_store.total_duration_ms = 0
    fake_module.FrameStore.return_value = fake_store

    fake_session = MagicMock()
    fake_module.RecordSession.prepare.return_value = fake_session

    with patch.object(frame_recorder_module, "gifrecorder", fake_module), \
         patch.object(frame_recorder_module, "_gifrecorder_available", True):
        yield fake_module, fake_store, fake_session


# 注意：下面几个替身一律用 `new=` 传入普通函数，不能写成
# patch.object(FrameRecorder, "...", return_value=X)。
# 后者会把一个 MagicMock 装到 FrameRecorder 这个 QObject 子类的类属性上，
# 之后 __init__ 里的 self._timer.timeout.connect(self._tick) 在遍历类的
# Qt 元信息时会踩到 Shiboken 内部结构，直接抛 Windows access violation，
# 把整个 pytest 进程杀掉（后续测试文件全部不再执行）。稳定复现。

@pytest.fixture(autouse=True)
def mock_win32_cursor():
    """所有测试默认屏蔽真实 GetCursorPos/GetAsyncKeyState 调用"""
    with patch.object(FrameRecorder, "_get_cursor_pos",
                      new=staticmethod(lambda: (0, 0))), \
         patch.object(FrameRecorder, "_is_left_pressed",
                      new=staticmethod(lambda: False)), \
         patch.object(FrameRecorder, "_is_right_pressed",
                      new=staticmethod(lambda: False)):
        yield


@pytest.fixture(autouse=True)
def hdr_handoff():
    """录制开始/结束时对截图会话的借出与交还；不碰进程级的 _HdrSession 状态。"""
    from concurrent.futures import Future

    events = []

    def lend(then=None):
        events.append("lend")
        done = Future()
        done.set_result(then() if then else None)
        return done

    def give_back(after=None):
        if after is not None:
            after()
        events.append("return")

    with patch.object(frame_recorder_module, "lend_hdr_session", lend), \
         patch.object(frame_recorder_module, "return_hdr_session", give_back):
        yield events


class TestHdrSessionHandoff:
    """同一进程每块屏只能有一个 DXGI 会话：录制窗口开着期间截图会话借给录制线程，关窗口时交还。"""

    @pytest.fixture(autouse=True)
    def hdr_display_on(self):
        with patch("capture.capture_service.hdr_display_active", return_value=True) as active:
            yield active

    def _recorder(self, engine):
        from settings.tool_settings import get_tool_settings_manager
        get_tool_settings_manager().set_capture_engine(engine)
        recorder = FrameRecorder()
        recorder.set_rect(QRect(0, 0, 200, 200))
        return recorder

    def test_prepare_lends_session_and_readies_the_dxgi_thread(self, qapp, mock_gifrecorder, hdr_handoff):
        fake_module, _, fake_session = mock_gifrecorder
        recorder = self._recorder("auto")

        recorder.prepare()
        assert hdr_handoff == ["lend"]
        fake_module.RecordSession.prepare.assert_called_once_with(prefer_dxgi=True)
        fake_session.begin.assert_not_called()

        recorder.start()
        fake_session.begin.assert_called_once()
        fake_module.RecordSession.prepare.assert_called_once()
        recorder.stop()
        assert hdr_handoff == ["lend"], "停止录制后窗口还开着，会话继续借着"

    def test_start_without_prepare_prepares_on_demand(self, qapp, mock_gifrecorder, hdr_handoff):
        _, _, fake_session = mock_gifrecorder
        recorder = self._recorder("auto")
        recorder.start()

        assert recorder.state == RecordState.RECORDING
        assert hdr_handoff == ["lend"]
        fake_session.begin.assert_called_once()
        recorder.stop()

    def test_mss_engine_records_with_gdi_and_keeps_session(self, qapp, mock_gifrecorder, hdr_handoff):
        fake_module, _, _ = mock_gifrecorder
        recorder = self._recorder("mss")
        recorder.prepare()
        recorder.start()
        recorder.stop()
        recorder.release()

        fake_module.RecordSession.prepare.assert_called_once_with(prefer_dxgi=False)
        assert hdr_handoff == []

    def test_auto_on_sdr_displays_records_with_gdi_and_keeps_session(
            self, qapp, mock_gifrecorder, hdr_handoff, hdr_display_on):
        fake_module, _, _ = mock_gifrecorder
        hdr_display_on.return_value = False
        recorder = self._recorder("auto")
        recorder.prepare()
        recorder.start()
        recorder.stop()
        recorder.release()

        fake_module.RecordSession.prepare.assert_called_once_with(prefer_dxgi=False)
        assert hdr_handoff == []

    def test_rerecording_prepares_again_without_lending_twice(self, qapp, mock_gifrecorder, hdr_handoff):
        fake_module, _, _ = mock_gifrecorder
        recorder = self._recorder("auto")
        recorder.prepare()
        recorder.start()
        recorder.stop()
        recorder.reset()
        recorder.prepare()

        assert fake_module.RecordSession.prepare.call_count == 2
        assert hdr_handoff == ["lend"]

    @pytest.mark.parametrize("recorded", [False, True])
    def test_release_stops_the_thread_before_returning_the_session(
            self, qapp, mock_gifrecorder, hdr_handoff, recorded):
        _, _, fake_session = mock_gifrecorder
        recorder = self._recorder("auto")
        recorder.prepare()
        if recorded:
            recorder.start()
        fake_session.stop.side_effect = lambda: hdr_handoff.append("stopped")

        recorder.release()

        assert hdr_handoff == ["lend", "stopped", "return"]

    def test_failed_begin_leaves_recorder_idle(self, qapp, mock_gifrecorder, hdr_handoff):
        _, _, fake_session = mock_gifrecorder
        fake_session.begin.side_effect = RuntimeError("recording thread has exited")
        recorder = self._recorder("auto")
        recorder.start()

        assert recorder.state == RecordState.IDLE
        fake_session.stop.assert_called_once()

    def test_async_stop_keeps_session_lent(self, qapp, qtbot, mock_gifrecorder, hdr_handoff):
        _, _, fake_session = mock_gifrecorder
        recorder = self._recorder("auto")
        recorder.start()
        fake_session.stop.side_effect = lambda: hdr_handoff.append("stopped")

        with qtbot.waitSignal(recorder.stop_finished, timeout=3000):
            recorder.stop_async()

        assert hdr_handoff == ["lend", "stopped"]


class TestRecordStateMachine:
    def test_initial_state_is_idle(self, qapp):
        recorder = FrameRecorder()
        assert recorder.state == RecordState.IDLE

    def test_start_transitions_to_recording(self, qapp, mock_gifrecorder):
        recorder = FrameRecorder()
        recorder.set_rect(QRect(0, 0, 200, 200))
        recorder.start()
        assert recorder.state == RecordState.RECORDING
        recorder._timer.stop()  # 清理，避免测试后台计时器继续跑

    def test_start_when_gifrecorder_unavailable_stays_idle(self, qapp):
        with patch.object(frame_recorder_module, "_gifrecorder_available", False):
            recorder = FrameRecorder()
            recorder.set_rect(QRect(0, 0, 200, 200))
            recorder.start()
        assert recorder.state == RecordState.IDLE

    def test_start_ignored_when_already_recording(self, qapp, mock_gifrecorder):
        _, _, _ = mock_gifrecorder
        recorder = FrameRecorder()
        recorder.set_rect(QRect(0, 0, 200, 200))
        recorder.start()
        first_store = recorder.store
        recorder.start()  # 第二次调用应被忽略（状态非 IDLE/STOPPED）
        assert recorder.store is first_store
        recorder._timer.stop()

    def test_pause_transitions_to_paused(self, qapp, mock_gifrecorder):
        recorder = FrameRecorder()
        recorder.set_rect(QRect(0, 0, 200, 200))
        recorder.start()
        recorder.pause()
        assert recorder.state == RecordState.PAUSED

    def test_pause_ignored_when_not_recording(self, qapp):
        recorder = FrameRecorder()
        recorder.pause()
        assert recorder.state == RecordState.IDLE

    def test_resume_transitions_back_to_recording(self, qapp, mock_gifrecorder):
        recorder = FrameRecorder()
        recorder.set_rect(QRect(0, 0, 200, 200))
        recorder.start()
        recorder.pause()
        recorder.resume()
        assert recorder.state == RecordState.RECORDING
        recorder._timer.stop()

    def test_resume_ignored_when_not_paused(self, qapp):
        recorder = FrameRecorder()
        recorder.resume()
        assert recorder.state == RecordState.IDLE

    def test_stop_transitions_to_stopped(self, qapp, mock_gifrecorder):
        recorder = FrameRecorder()
        recorder.set_rect(QRect(0, 0, 200, 200))
        recorder.start()
        frames = recorder.stop()
        assert recorder.state == RecordState.STOPPED
        assert frames == recorder.frames

    def test_reset_returns_to_idle(self, qapp, mock_gifrecorder):
        recorder = FrameRecorder()
        recorder.set_rect(QRect(0, 0, 200, 200))
        recorder.start()
        recorder.stop()
        recorder.reset()
        assert recorder.state == RecordState.IDLE
        assert recorder.frames == []
        assert recorder.store is None

    def test_state_changed_signal_emitted_on_start(self, qapp, mock_gifrecorder):
        recorder = FrameRecorder()
        recorder.set_rect(QRect(0, 0, 200, 200))
        received = []
        recorder.state_changed.connect(received.append)
        recorder.start()
        assert "RECORDING" in received
        recorder._timer.stop()


class TestRectInset:
    """录制区域会内缩1px避免录入选区边框线，最小2px"""

    def test_normal_rect_insets_by_one_pixel_each_side(self, qapp, mock_gifrecorder):
        recorder = FrameRecorder()
        recorder.set_rect(QRect(100, 50, 202, 102))  # width=202, height=102
        recorder.start()
        w, h = recorder.rec_size
        assert w == 200  # 202 - 2*1
        assert h == 100  # 102 - 2*1
        recorder._timer.stop()

    def test_tiny_rect_clamped_to_minimum_two_pixels(self, qapp, mock_gifrecorder):
        recorder = FrameRecorder()
        recorder.set_rect(QRect(0, 0, 2, 2))  # 内缩后会变成0，应clamp到2
        recorder.start()
        w, h = recorder.rec_size
        assert w == 2
        assert h == 2
        recorder._timer.stop()

    def test_rec_left_top_offset_by_inset(self, qapp, mock_gifrecorder):
        fake_module, fake_store, fake_session = mock_gifrecorder
        recorder = FrameRecorder()
        recorder.set_rect(QRect(300, 400, 202, 102))
        recorder.start()
        # 录制会话应以内缩后的 left/top 开始: (300+1, 400+1)
        fake_session.begin.assert_called_once()
        call_args = fake_session.begin.call_args[0]
        # 签名: (store, left, top, w, h, fps)
        assert call_args[1] == 301
        assert call_args[2] == 401
        recorder._timer.stop()


class TestSyncFramesFromStore:
    def test_no_store_leaves_frames_unchanged(self, qapp):
        recorder = FrameRecorder()
        recorder._sync_frames_from_store()
        assert recorder.frames == []

    def test_frame_count_matches_timestamps(self, qapp, mock_gifrecorder):
        _, fake_store, _ = mock_gifrecorder
        recorder = FrameRecorder()
        recorder._store = fake_store
        recorder._rec_width = 640
        recorder._rec_height = 480
        fake_store.frame_timestamps = [0, 33, 66, 100]
        recorder._cursor_track = []

        recorder._sync_frames_from_store()

        assert len(recorder.frames) == 4
        assert all(isinstance(f, FrameData) for f in recorder.frames)
        assert recorder.frames[0].elapsed_ms == 0
        assert recorder.frames[-1].elapsed_ms == 100
        assert all(f.width == 640 and f.height == 480 for f in recorder.frames)

    def test_cursor_track_aligned_one_to_one_when_counts_match(self, qapp, mock_gifrecorder):
        _, fake_store, _ = mock_gifrecorder
        recorder = FrameRecorder()
        recorder._store = fake_store
        fake_store.frame_timestamps = [0, 10, 20]
        recorder._cursor_track = [
            CursorSnapshot(x=1, y=1, press=0),
            CursorSnapshot(x=2, y=2, press=1),
            CursorSnapshot(x=3, y=3, press=0),
        ]

        recorder._sync_frames_from_store()

        assert [f.cursor.x for f in recorder.frames] == [1, 2, 3]

    def test_cursor_track_shorter_than_frames_uses_proportional_mapping(self, qapp, mock_gifrecorder):
        """鼠标采样数少于帧数时（QTimer与Rust线程速率不完全一致），按比例映射"""
        _, fake_store, _ = mock_gifrecorder
        recorder = FrameRecorder()
        recorder._store = fake_store
        fake_store.frame_timestamps = [0, 10, 20, 30]  # 4帧
        recorder._cursor_track = [
            CursorSnapshot(x=100, y=100, press=0),
            CursorSnapshot(x=200, y=200, press=0),
        ]  # 只有2个采样

        recorder._sync_frames_from_store()

        assert len(recorder.frames) == 4
        # 每一帧都应绑定到某个有效的cursor采样（不应为None，不应越界抛异常）
        for f in recorder.frames:
            assert f.cursor is not None
            assert f.cursor.x in (100, 200)

    def test_no_cursor_track_leaves_cursor_none(self, qapp, mock_gifrecorder):
        _, fake_store, _ = mock_gifrecorder
        recorder = FrameRecorder()
        recorder._store = fake_store
        fake_store.frame_timestamps = [0, 10]
        recorder._cursor_track = []

        recorder._sync_frames_from_store()

        assert all(f.cursor is None for f in recorder.frames)

    def test_sync_clears_previous_frames_first(self, qapp, mock_gifrecorder):
        _, fake_store, _ = mock_gifrecorder
        recorder = FrameRecorder()
        recorder._store = fake_store
        recorder._frames = [FrameData(elapsed_ms=999)]  # 陈旧数据
        fake_store.frame_timestamps = [0, 5]
        recorder._cursor_track = []

        recorder._sync_frames_from_store()

        assert len(recorder.frames) == 2
        assert 999 not in [f.elapsed_ms for f in recorder.frames]


class TestMaxDurationAutoStop:
    def test_tick_emits_limit_reached_after_max_duration(self, qapp, mock_gifrecorder):
        recorder = FrameRecorder()
        recorder.set_rect(QRect(0, 0, 200, 200))
        recorder.start()
        # 0/0.0 在源码里被当作"不限制"（`if self._max_duration_s > 0 and ...`），
        # 所以用一个极小的正数并把开始时间往前拨，确保 elapsed >= max_duration_s。
        recorder._max_duration_s = 0.001
        recorder._start_time -= 1.0

        received = []
        recorder.limit_reached.connect(lambda: received.append(True))
        recorder._tick()

        assert received == [True]
        recorder._timer.stop()

    def test_tick_does_not_stop_when_duration_unlimited(self, qapp, mock_gifrecorder):
        recorder = FrameRecorder()
        recorder.set_rect(QRect(0, 0, 200, 200))
        recorder._max_duration_s = 0  # 0 = 不限制
        recorder.start()

        received = []
        recorder.limit_reached.connect(lambda: received.append(True))
        recorder._tick()

        assert received == []
        recorder._timer.stop()

    def test_frame_captured_signal_emits_current_count(self, qapp, mock_gifrecorder):
        _, fake_store, _ = mock_gifrecorder
        recorder = FrameRecorder()
        recorder.set_rect(QRect(0, 0, 200, 200))
        recorder.start()
        fake_store.frame_count = 7

        received = []
        recorder.frame_captured.connect(received.append)
        recorder._tick()

        assert received == [7]
        recorder._timer.stop()


class TestSetFps:
    def test_set_fps_updates_property(self, qapp):
        recorder = FrameRecorder()
        recorder.set_fps(30)
        assert recorder.fps == 30

    def test_set_fps_updates_timer_interval_while_recording(self, qapp, mock_gifrecorder):
        recorder = FrameRecorder()
        recorder.set_rect(QRect(0, 0, 200, 200))
        recorder.start()
        recorder.set_fps(30)
        assert recorder._timer.interval() == 1000 // 30
        recorder._timer.stop()


class TestResetData:
    def test_reset_data_clears_store_and_frames_without_touching_timer(self, qapp, mock_gifrecorder):
        recorder = FrameRecorder()
        recorder.set_rect(QRect(0, 0, 200, 200))
        recorder.start()
        recorder._timer.stop()  # 模拟已停止的timer状态
        recorder.reset_data()
        assert recorder.store is None
        assert recorder.frames == []
        assert recorder.state == RecordState.IDLE


class TestScrollTrack:
    def _wheel(self, delta, horizontal=False):
        input_hub().native.mouse("hwheel" if horizontal else "wheel", 50, 60, delta=delta)

    def test_wheel_during_recording_marks_the_next_snapshot_once(self, qapp, mock_gifrecorder):
        recorder = FrameRecorder()
        recorder.set_rect(QRect(0, 0, 200, 200))
        recorder.start()
        self._wheel(-120)
        self._wheel(40, horizontal=True)  # 横向滚轮不记
        qapp.processEvents()
        recorder._sample_cursor()
        self._wheel(120)
        qapp.processEvents()
        recorder._sample_cursor()
        recorder._sample_cursor()
        assert [snapshot.scroll for snapshot in recorder._cursor_track] == [-1, 1, 0]
        recorder._timer.stop()

    def test_listening_stops_with_the_recording(self, qapp, mock_gifrecorder):
        recorder = FrameRecorder()
        recorder.set_rect(QRect(0, 0, 200, 200))
        recorder.start()
        assert input_hub().native.hooks_needed
        recorder.stop()
        assert not input_hub().native.hooks_needed
        self._wheel(-120)
        qapp.processEvents()
        assert recorder._scroll_value == 0

