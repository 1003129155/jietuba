"""当前源码的 PyO3 录制 worker 进程测试，不采集真实桌面或系统声音。"""

import json
import os
from pathlib import Path
import queue
import struct
import subprocess
import sys
import threading
import time

import av
import pytest


@pytest.fixture(scope="module")
def helper():
    if os.environ.get("JIETUBA_VIDEO_TEST_NATIVE") != "1":
        pytest.skip("需要从当前源码安装 test-support 扩展；CI 两种架构必跑")
    import video_recorder
    assert hasattr(video_recorder.Recorder, "_configure_test"), "必须安装本次源码的测试扩展"
    worker = Path(__file__).resolve().parents[1] / "video_worker.py"
    return [sys.executable, "-u", str(worker)]


class Session:
    def __init__(self, helper, output, *extra):
        self.events = []
        self.lines = queue.Queue()
        self.process = subprocess.Popen([
            *helper, "--synthetic", "--output", str(output), "--left", "0", "--top", "0",
            "--width", "320", "--height", "240", "--fps", "17", "--bitrate", "1000000",
            "--audio", "none", "--encoder", "software", "--cursor", "off", *extra,
        ], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.stderr = bytearray()
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.errors = threading.Thread(target=self._stderr, daemon=True)
        self.reader.start()
        self.errors.start()

    def _read(self):
        for line in self.process.stdout:
            self.lines.put(line)
        self.lines.put(None)

    def _stderr(self):
        while chunk := self.process.stderr.read(4096):
            self.stderr.extend(chunk)
            del self.stderr[:-16384]

    def wait_event(self, name, timeout=25):
        deadline = time.monotonic() + timeout
        while True:
            line = self.lines.get(timeout=max(0.01, deadline - time.monotonic()))
            assert line is not None, bytes(self.stderr).decode(errors="replace")
            assert len(line) <= 16384
            event = json.loads(line)
            assert event["protocol"] == 1 and isinstance(event["data"], dict)
            self.events.append(event)
            if event["event"] == name:
                return event["data"]
            assert event["event"] != "error", event

    def send(self, command):
        self.process.stdin.write((json.dumps({"protocol": 1, "command": command}) + "\n").encode())
        self.process.stdin.flush()

    def exit(self):
        result = self.process.wait(timeout=10)
        self.reader.join(5)
        self.errors.join(5)
        return result

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait(timeout=5)
        for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
            stream.close()


def mp4_boxes(path):
    result = []
    with path.open("rb") as stream:
        while header := stream.read(8):
            assert len(header) == 8
            size, name = struct.unpack(">I4s", header)
            if size == 1:
                size = struct.unpack(">Q", stream.read(8))[0]
                consumed = 16
            else:
                consumed = 8
            assert size >= consumed
            result.append(name)
            stream.seek(size - consumed, 1)
    return result


def test_source_helper_finalizes_custom_fps_in_chinese_path(helper, tmp_path):
    output = tmp_path / "录制结果.mp4"
    session = Session(helper, output, "--duration", "2")
    try:
        ready = session.wait_event("ready")
        assert ready["fps"] == 17 and ready["bitrate"] == 1000000
        assert ready["hardware"] is False
        complete = session.wait_event("complete")
        assert complete["finalized"] is True and Path(complete["output"]) == output
        assert 25 <= complete["frames"] <= 35
        assert session.exit() == 0
        assert b"ftyp" in mp4_boxes(output) and b"moov" in mp4_boxes(output)
    finally:
        session.close()


def test_pause_resume_stop_and_eof_have_confirmed_process_exit(helper, tmp_path):
    session = Session(helper, tmp_path / "paused.mp4")
    try:
        session.wait_event("ready")
        session.wait_event("progress")
        session.send("pause")
        paused = session.wait_event("paused")
        time.sleep(0.2)
        session.send("resume")
        resumed = session.wait_event("resumed")
        assert abs(paused["elapsed_ms"] - resumed["elapsed_ms"]) < 50
        # 父进程正常关闭控制管道，也必须先完成 MP4 封装再退出。
        session.process.stdin.close()
        assert session.wait_event("complete")["finalized"] is True
        assert session.exit() == 0
    finally:
        session.close()


def test_cancel_removes_only_new_partial_output(helper, tmp_path):
    output = tmp_path / "cancel.mp4"
    session = Session(helper, output)
    try:
        session.wait_event("ready")
        session.wait_event("progress")
        session.send("cancel")
        session.wait_event("cancelled")
        assert session.exit() == 0 and not output.exists()
    finally:
        session.close()


def test_stop_without_video_frames_cancels_and_removes_empty_output(helper, tmp_path):
    output = tmp_path / "no-frames.mp4"
    # ready 的协议输出已超过此时限，保证进入正常循环收尾但未采集第一帧。
    session = Session(helper, output, "--duration", "0.000000001")
    try:
        session.wait_event("ready")
        assert session.wait_event("cancelled")["frames"] == 0
        assert session.exit() == 0 and not output.exists()
    finally:
        session.close()


def test_stop_exits_even_if_parent_keeps_stdin_open(helper, tmp_path):
    session = Session(helper, tmp_path / "stopped.mp4")
    try:
        session.wait_event("ready")
        session.wait_event("progress")
        session.send("stop")
        assert session.wait_event("complete")["finalized"] is True
        assert not session.process.stdin.closed
        assert session.exit() == 0
    finally:
        session.close()


def assert_video_ends_at_stop(output, complete):
    with av.open(str(output)) as container:
        stream = container.streams.video[0]
        duration = float(stream.duration * stream.time_base)
        frames = list(container.decode(video=0))
        timestamps = [float(frame.pts * frame.time_base) for frame in frames]
    assert len(frames) == complete["frames"] > 0
    assert timestamps == sorted(set(timestamps))
    # MP4 时间基允许毫秒级舍入，不能用一个完整帧间隔作误差，掩盖低帧率尾画面。
    assert abs(duration - complete["elapsed_ms"] / 1000) < 0.015


@pytest.mark.parametrize("fps,duration", [(1, 0.2), (1, 1.2), (17, 0.23), (60, 0.23)])
def test_fractional_recording_duration_is_preserved_in_decoded_mp4(helper, tmp_path, fps, duration):
    output = tmp_path / "fractional.mp4"
    session = Session(helper, output, "--fps", str(fps), "--duration", str(duration))
    try:
        session.wait_event("ready")
        complete = session.wait_event("complete")
        assert session.exit() == 0
        assert_video_ends_at_stop(output, complete)
    finally:
        session.close()


@pytest.mark.parametrize("action", ["stop", "pause_stop", "pause_resume_stop"])
def test_low_fps_last_frame_ends_at_actual_stop_excluding_pause(helper, tmp_path, action):
    output = tmp_path / "last-frame.mp4"
    session = Session(helper, output, "--fps", "1")
    try:
        session.wait_event("ready")
        session.wait_event("progress")
        if action == "stop":
            time.sleep(1.2)
        else:
            time.sleep(0.2)
            session.send("pause")
            paused = session.wait_event("paused")
            time.sleep(0.2)
            if action == "pause_resume_stop":
                session.send("resume")
                session.wait_event("resumed")
                time.sleep(0.2)
        session.send("stop")
        complete = session.wait_event("complete")
        assert session.exit() == 0
        if action == "pause_stop":
            assert complete["elapsed_ms"] == paused["elapsed_ms"]
        assert_video_ends_at_stop(output, complete)
    finally:
        session.close()


def test_protocol_failure_preserves_the_unsubmitted_first_frame(helper, tmp_path):
    output = tmp_path / "partial-first-frame.mp4"
    session = Session(helper, output, "--fps", "1")
    try:
        session.wait_event("ready")
        session.wait_event("progress")
        time.sleep(0.2)
        session.process.stdin.write(b"invalid\n")
        session.process.stdin.flush()
        assert session.wait_event("error")["code"] == "protocol"
        assert session.exit() != 0
        with av.open(str(output)) as container:
            stream = container.streams.video[0]
            duration = float(stream.duration * stream.time_base)
            assert len(list(container.decode(video=0))) == 1
        assert 0.15 < duration < 0.8
    finally:
        session.close()


def test_hardware_initialization_failure_uses_real_system_software_encoder(helper, tmp_path):
    session = Session(helper, tmp_path / "fallback.mp4", "--encoder", "auto",
                      "--fail-hardware-init", "--duration", "1")
    try:
        assert session.wait_event("ready")["hardware"] is False
        assert session.wait_event("complete")["finalized"] is True
        assert session.exit() == 0
        assert b"software_fallback" in session.stderr
    finally:
        session.close()


@pytest.mark.parametrize("case", [
    "existing", "fps", "bitrate", "protocol", "oversized",
    "protocol_at_start", "oversized_at_start",
])
def test_rejections_do_not_report_a_successful_video(helper, tmp_path, case):
    output = tmp_path / "failure.mp4"
    extra = []
    if case == "existing":
        output.write_bytes(b"user file")
    elif case == "fps":
        extra = ["--fps", "0"]
    elif case == "bitrate":
        extra = ["--bitrate", "499999"]
    elif case.endswith("_at_start"):
        extra = ["--encoder", "auto", "--fail-hardware-init"]
    session = Session(helper, output, *extra)
    try:
        if case.startswith(("protocol", "oversized")):
            if not case.endswith("_at_start"):
                session.wait_event("ready")
            # 初始化期间也应明确报告协议错误，不要求竞争条件必然落在回退路径。
            session.process.stdin.write(b"invalid\n" if case.startswith("protocol") else b"x" * 2049 + b"\n")
            session.process.stdin.flush()
        error = session.wait_event("error")
        assert error["code"] in ("output_exists", "arguments", "protocol")
        assert session.exit() != 0
        assert not any(event["event"] == "complete" for event in session.events)
        if case == "existing":
            assert output.read_bytes() == b"user file"
    finally:
        session.close()


@pytest.mark.skipif(os.environ.get("JIETUBA_VIDEO_TEST_AUDIO") != "1",
                    reason="本机系统声音验收，需要播放测试音和可用的播放设备")
def test_real_loopback_audio_survives_pause_and_matches_video_duration(helper, tmp_path):
    from array import array
    import math
    import wave
    import winsound

    tone = tmp_path / "loopback-tone.wav"
    samples = bytearray()
    for index in range(48_000):
        samples.extend(struct.pack("<hh", int(3000 * math.sin(2 * math.pi * 440 * index / 48_000)),
                                   int(3000 * math.sin(2 * math.pi * 880 * index / 48_000))))
    with wave.open(str(tone), "wb") as output:
        output.setnchannels(2)
        output.setsampwidth(2)
        output.setframerate(48_000)
        output.writeframes(samples)
    session = None
    try:
        winsound.PlaySound(str(tone), winsound.SND_FILENAME | winsound.SND_ASYNC | winsound.SND_LOOP)
        session = Session(helper, tmp_path / "system-audio.mp4", "--audio", "system")
        ready = session.wait_event("ready")
        assert ready["audio"] == "system"
        session.wait_event("progress")
        session.wait_event("progress")
        session.send("pause")
        paused = session.wait_event("paused")
        time.sleep(0.35)
        session.send("resume")
        resumed = session.wait_event("resumed")
        assert abs(paused["elapsed_ms"] - resumed["elapsed_ms"]) < 50
        session.wait_event("progress")
        session.send("stop")
        complete = session.wait_event("complete")
        assert session.exit() == 0
        path = tmp_path / "system-audio.mp4"
        with av.open(str(path)) as recording:
            video = recording.streams.video[0]
            audio = recording.streams.audio[0]
            assert audio.codec_context.name == "aac" and audio.codec_context.sample_rate == 48_000
            assert len(audio.codec_context.layout.channels) == 2
            assert abs(float(video.duration * video.time_base) - complete["elapsed_ms"] / 1000) < 0.1
            assert abs(float(audio.duration * audio.time_base) - float(video.duration * video.time_base)) < 0.1
            before, after = [], []
            for frame in recording.decode(audio):
                assert frame.format.name == "fltp"
                samples = array("f")
                for plane in frame.planes:
                    samples.frombytes(bytes(plane)[:frame.samples * 4])
                rms = math.sqrt(sum(value * value for value in samples) / len(samples))
                seconds = float(frame.pts * frame.time_base)
                if 0.2 < seconds < paused["elapsed_ms"] / 1000 - 0.1:
                    before.append(rms)
                if paused["elapsed_ms"] / 1000 + 0.2 < seconds < complete["elapsed_ms"] / 1000 - 0.1:
                    after.append(rms)
            assert before and after and max(before) > 0.001 and max(after) > 0.001
        assert b"audio_stopped" in session.stderr
    finally:
        winsound.PlaySound(None, 0)
        if session is not None:
            session.close()
