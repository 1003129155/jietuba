"""真实 Rust 扩展的定位、帧内容和回放结束测试。"""

import time

import pytest

gifrecorder = pytest.importorskip("gifrecorder")
DIRECT_SEEK = "start_frame" in (gifrecorder.FrameStore.start_decoder.__text_signature__ or "")
requires_direct_seek = pytest.mark.skipif(
    not DIRECT_SEEK, reason="requires locally rebuilt gifrecorder with start_frame"
)


@pytest.fixture
def store():
    result = gifrecorder.FrameStore(16, 16, 10)
    for i in range(6):
        result.push_rgb(bytes([30 + i * 30]) * (16 * 16 * 3), i * 100)
    return result


@requires_direct_seek
@pytest.mark.parametrize("start", [0, 3, 5, 6, 100])
def test_direct_decoder_returns_original_frames_and_timestamps(store, start):
    decoder = store.start_decoder(8, 8, 2, start_frame=start)
    try:
        assert decoder.total_frames == max(0, 6 - start)
        assert decoder.fetched_count == 0
        for index in range(start, 6):
            rgb, elapsed = decoder.next_frame()
            assert rgb == store.get_frame_rgb(index, 8, 8)
            assert elapsed == index * 100
        assert decoder.next_frame() is None
        assert decoder.is_finished
        assert decoder.fetched_count == max(0, 6 - start)
    finally:
        decoder.stop()


@requires_direct_seek
def test_legacy_positional_arguments_still_start_at_zero(store):
    decoder = store.start_decoder(16, 16, 1)
    try:
        assert decoder.next_frame()[1] == 0
    finally:
        decoder.stop()


@requires_direct_seek
def test_skip_counts_from_the_selected_start(store):
    decoder = store.start_decoder(start_frame=3)
    try:
        assert decoder.skip(1) == 1
        assert decoder.next_frame()[1] == 400
        assert decoder.skip(100) == 1
        assert decoder.is_finished
    finally:
        decoder.stop()


@pytest.mark.parametrize("trim", [(3, 5), (5, 5), (2, 4)])
def test_real_engine_seek_pause_speed_trim_and_end(qapp, store, trim):
    from gif.frame_recorder import FrameData
    from gif.playback_engine import PlaybackEngine, PlayState

    engine = PlaybackEngine()
    seen = []
    finished = []
    engine.frame_ready.connect(lambda image, index: seen.append((image.copy(), index)))
    engine.playback_finished.connect(lambda: finished.append(True))
    engine.load([FrameData(elapsed_ms=i * 100, width=16, height=16) for i in range(6)], 10, store)
    engine.set_display_size(16, 16)
    try:
        engine.play()
        engine.seek(trim[0])
        engine.pause()
        engine.play()
        engine.set_speed(2)
        engine.set_trim(*trim)
        # 手动取帧以排除墙钟调度抖动；仍走真实 Rust 缓冲及引擎信号。
        engine.stop_timer()
        deadline = time.monotonic() + 5
        while not finished and time.monotonic() < deadline:
            engine._advance()
            time.sleep(0.001)
        assert finished == [True]
        assert engine.state == PlayState.IDLE
        assert [index for _, index in seen] == list(range(trim[0], trim[1] + 1))
        for image, index in seen:
            assert bytes(image.constBits()) == store.get_frame_rgb(index, 16, 16)
    finally:
        engine.stop()
