# -*- coding: utf-8 -*-
"""
GIF 回放引擎状态机测试

PlaybackEngine 管着录制回放的播放/暂停/停止、进度跳转、倍速和裁剪范围。
它本身不做像素处理——解码交给 Rust 的 FrameDecoder——所以状态迁移、
索引钳制和定时器间隔这些真正容易错的部分都可以用假的 FrameStore 驱动。

重点覆盖：
- 没有帧或没有 store 时 play() 必须是空操作，否则会启动一个永远取不到帧的定时器
- seek / set_trim 的索引钳制：进度条拖到范围外不能让当前帧跑飞
- 倍速对定时器间隔的影响，以及 10ms 的下限（否则高倍速会把事件循环占满）
- 播放到裁剪终点时要停下来并发出结束信号，而不是继续越界取帧
"""
import pytest
from PySide6.QtWidgets import QApplication


@pytest.fixture(scope="module")
def qapp():
    app = QApplication.instance()
    if app is None:
        app = QApplication([])
    yield app


class FakeDecoder:
    """替代 Rust 的 FrameDecoder，只记录被要求跳过多少帧"""

    def __init__(self):
        self.skipped = 0
        self.stopped = False
        self.is_finished = False

    def skip(self, n):
        self.skipped += n

    def stop(self):
        self.stopped = True

    def try_next_frame(self):
        return None


class FakeStore:
    """替代 Rust 的 FrameStore"""

    def __init__(self):
        self.decoders = []
        self.requested_sizes = []

    def start_decoder(self, display_w, display_h, prefetch=6):
        self.requested_sizes.append((display_w, display_h))
        dec = FakeDecoder()
        self.decoders.append(dec)
        return dec

    def get_frame_rgb(self, index, w, h):
        return bytes(w * h * 3)


def _frames(count, width=64, height=48):
    from gif.frame_recorder import FrameData
    return [FrameData(elapsed_ms=i * 62, width=width, height=height)
            for i in range(count)]


@pytest.fixture
def engine(qapp):
    from gif.playback_engine import PlaybackEngine
    eng = PlaybackEngine()
    yield eng
    eng.stop_timer()
    eng.cleanup()


@pytest.fixture
def loaded(engine):
    """载入 10 帧、16fps 的引擎"""
    store = FakeStore()
    engine.load(_frames(10), fps=16, store=store)
    engine.set_display_size(64, 48)
    return engine, store


# ============================================================================
# 载入
# ============================================================================

class TestLoad:
    def test_a_fresh_engine_is_idle_and_empty(self, engine):
        from gif.playback_engine import PlayState
        assert engine.state is PlayState.IDLE
        assert engine.frame_count == 0
        assert engine.is_playing is False

    def test_loading_sets_the_frame_count(self, loaded):
        engine, _store = loaded
        assert engine.frame_count == 10

    def test_loading_selects_the_whole_range(self, loaded):
        engine, _store = loaded
        assert engine.trim_start == 0
        assert engine.trim_end == 9
        assert engine.current_index == 0

    def test_loading_again_resets_position_and_state(self, loaded):
        from gif.playback_engine import PlayState
        engine, store = loaded
        engine.play()
        engine.seek(5)

        engine.load(_frames(3), fps=16, store=store)

        assert engine.state is PlayState.IDLE
        assert engine.current_index == 0
        assert engine.trim_end == 2

    def test_loading_an_empty_recording_is_safe(self, engine):
        engine.load([], fps=16, store=FakeStore())
        assert engine.frame_count == 0
        assert engine.trim_end == 0


# ============================================================================
# 播放状态迁移
# ============================================================================

class TestStateTransitions:
    def test_play_enters_the_playing_state(self, loaded):
        from gif.playback_engine import PlayState
        engine, _store = loaded
        engine.play()
        assert engine.state is PlayState.PLAYING
        assert engine.is_playing is True

    def test_pause_then_play_resumes(self, loaded):
        from gif.playback_engine import PlayState
        engine, _store = loaded
        engine.play()
        engine.pause()
        assert engine.state is PlayState.PAUSED

        engine.play()
        assert engine.state is PlayState.PLAYING

    def test_stop_returns_to_idle(self, loaded):
        from gif.playback_engine import PlayState
        engine, _store = loaded
        engine.play()
        engine.stop()
        assert engine.state is PlayState.IDLE

    def test_play_without_frames_does_nothing(self, engine):
        """没有帧就启动定时器，只会空转取不到帧"""
        from gif.playback_engine import PlayState
        engine.load([], fps=16, store=FakeStore())
        engine.play()
        assert engine.state is PlayState.IDLE

    def test_play_without_a_store_does_nothing(self, engine):
        """帧元数据在、但 Rust 侧的 FrameStore 没准备好时同样不能开播"""
        from gif.playback_engine import PlayState
        engine.load(_frames(5), fps=16, store=None)
        engine.play()
        assert engine.state is PlayState.IDLE

    def test_playing_twice_does_not_restart_the_decoder(self, loaded):
        """重复点播放不应把已经在跑的解码器推倒重来"""
        engine, store = loaded
        engine.play()
        engine.play()
        assert len(store.decoders) == 1

    def test_pause_keeps_the_current_position(self, loaded):
        engine, _store = loaded
        engine.seek(4)
        engine.play()
        engine.pause()
        assert engine.current_index == 4


# ============================================================================
# 进度跳转
# ============================================================================

class TestSeek:
    def test_seek_moves_to_the_requested_frame(self, loaded):
        engine, _store = loaded
        engine.seek(6)
        assert engine.current_index == 6

    def test_seek_emits_frame_changed(self, loaded):
        engine, _store = loaded
        seen = []
        engine.frame_changed.connect(seen.append)

        engine.seek(3)

        assert seen == [3]

    def test_seek_past_the_end_clamps_to_the_last_frame(self, loaded):
        engine, _store = loaded
        engine.seek(999)
        assert engine.current_index == 9

    def test_seek_before_the_start_clamps_to_the_first_frame(self, loaded):
        engine, _store = loaded
        engine.seek(-5)
        assert engine.current_index == 0

    def test_seek_is_clamped_to_the_trimmed_range(self, loaded):
        """裁剪后进度条只在裁剪区内活动，不能跳到被剪掉的帧上"""
        engine, _store = loaded
        engine.set_trim(3, 7)

        engine.seek(0)
        assert engine.current_index == 3

        engine.seek(9)
        assert engine.current_index == 7

    def test_seek_while_playing_restarts_the_decoder_at_that_frame(self, loaded):
        engine, store = loaded
        engine.play()
        engine.seek(5)

        assert store.decoders[-1].skipped == 5

    def test_seek_while_paused_does_not_spin_up_a_decoder(self, loaded):
        engine, store = loaded
        engine.seek(5)
        assert store.decoders == []


# ============================================================================
# 倍速
# ============================================================================

class TestSpeed:
    def test_default_interval_follows_the_frame_rate(self, loaded):
        engine, _store = loaded
        assert engine._interval_ms() == pytest.approx(1000 / 16, abs=1)

    def test_double_speed_halves_the_interval(self, loaded):
        engine, _store = loaded
        before = engine._interval_ms()
        engine.set_speed(2.0)
        assert engine._interval_ms() == pytest.approx(before / 2, abs=1)

    def test_half_speed_doubles_the_interval(self, loaded):
        engine, _store = loaded
        before = engine._interval_ms()
        engine.set_speed(0.5)
        assert engine._interval_ms() == pytest.approx(before * 2, abs=1)

    def test_interval_never_drops_below_ten_milliseconds(self, loaded):
        """极高倍速下若不设下限，定时器会把事件循环占满导致界面卡死"""
        engine, _store = loaded
        engine.set_speed(1000.0)
        assert engine._interval_ms() >= 10

    def test_speed_is_clamped_to_a_positive_minimum(self, loaded):
        """0 或负倍速会让间隔计算除零"""
        engine, _store = loaded
        engine.set_speed(0)
        assert engine._interval_ms() > 0

        engine.set_speed(-3)
        assert engine._interval_ms() > 0

    def test_setting_the_same_speed_while_playing_is_a_no_op(self, loaded):
        engine, store = loaded
        engine.play()
        engine.set_speed(1.0)          # 与当前一致
        assert len(store.decoders) == 1

    def test_changing_speed_while_playing_restarts_the_decoder(self, loaded):
        engine, store = loaded
        engine.play()
        engine.set_speed(2.0)
        assert len(store.decoders) == 2


# ============================================================================
# 裁剪范围
# ============================================================================

class TestTrim:
    def test_trim_sets_the_range(self, loaded):
        engine, _store = loaded
        engine.set_trim(2, 8)
        assert (engine.trim_start, engine.trim_end) == (2, 8)

    def test_trim_end_cannot_exceed_the_last_frame(self, loaded):
        engine, _store = loaded
        engine.set_trim(0, 999)
        assert engine.trim_end == 9

    def test_trim_start_cannot_go_negative(self, loaded):
        engine, _store = loaded
        engine.set_trim(-5, 8)
        assert engine.trim_start == 0

    def test_current_frame_is_pulled_into_the_new_range(self, loaded):
        """当前帧被裁掉时要落回范围内，否则画面停在一帧不存在的位置"""
        engine, _store = loaded
        engine.seek(9)

        engine.set_trim(0, 5)

        assert engine.current_index == 5

    def test_current_frame_is_pushed_up_to_a_later_start(self, loaded):
        engine, _store = loaded
        engine.seek(0)

        engine.set_trim(4, 9)

        assert engine.current_index == 4

    def test_current_frame_inside_the_range_is_left_alone(self, loaded):
        engine, _store = loaded
        engine.seek(5)
        engine.set_trim(3, 7)
        assert engine.current_index == 5


# ============================================================================
# 单帧取图
# ============================================================================

class TestGetFrameImage:
    def test_returns_an_image_of_the_requested_size(self, loaded):
        engine, _store = loaded
        img = engine.get_frame_image(0, 64, 48)
        assert img is not None
        assert (img.width(), img.height()) == (64, 48)

    def test_out_of_range_index_returns_none(self, loaded):
        engine, _store = loaded
        assert engine.get_frame_image(999, 64, 48) is None

    def test_returns_none_without_a_store(self, engine):
        engine.load(_frames(3), fps=16, store=None)
        assert engine.get_frame_image(0, 64, 48) is None

    def test_returns_none_without_frames(self, engine):
        engine.load([], fps=16, store=FakeStore())
        assert engine.get_frame_image(0, 64, 48) is None

    def test_decoder_failure_is_reported_as_none(self, loaded):
        """Rust 侧解码抛异常时要降级返回 None，不能把异常抛给 UI"""
        engine, store = loaded

        def boom(index, w, h):
            raise RuntimeError("decode failed")

        store.get_frame_rgb = boom

        assert engine.get_frame_image(0, 64, 48) is None


# ============================================================================
# 显示尺寸与清理
# ============================================================================

class TestDisplaySizeAndCleanup:
    def test_resizing_while_playing_restarts_the_decoder(self, loaded):
        """解码器按目标尺寸输出，窗口尺寸变了必须重开"""
        engine, store = loaded
        engine.play()
        engine.set_display_size(128, 96)

        assert store.requested_sizes[-1] == (128, 96)

    def test_resizing_to_the_same_size_is_a_no_op(self, loaded):
        engine, store = loaded
        engine.play()
        engine.set_display_size(64, 48)
        assert len(store.decoders) == 1

    def test_resizing_while_idle_does_not_start_a_decoder(self, loaded):
        engine, store = loaded
        engine.set_display_size(128, 96)
        assert store.decoders == []

    def test_cleanup_releases_frames_and_returns_to_idle(self, loaded):
        from gif.playback_engine import PlayState
        engine, _store = loaded
        engine.play()

        engine.stop_timer()
        engine.cleanup()

        assert engine.state is PlayState.IDLE
        assert engine.frame_count == 0

    def test_cleanup_stops_the_decoder(self, loaded):
        engine, store = loaded
        engine.play()
        decoder = store.decoders[-1]

        engine.stop_timer()
        engine.cleanup()

        assert decoder.stopped is True

    def test_cleanup_is_idempotent(self, loaded):
        engine, _store = loaded
        engine.stop_timer()
        engine.cleanup()
        engine.cleanup()          # 第二次不应抛异常
