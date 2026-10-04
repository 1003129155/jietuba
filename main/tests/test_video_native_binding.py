"""验证本次构建的 PyO3 接口及原生资源生命周期，使用合成画面。"""

import os
import subprocess
import sys
import textwrap

import pytest


@pytest.fixture(scope="module")
def native_binding():
    if os.environ.get("JIETUBA_VIDEO_TEST_NATIVE") != "1":
        pytest.skip("需要从当前源码安装 test-support 扩展；CI 两种架构必跑")
    import video_recorder
    assert hasattr(video_recorder.Recorder, "_configure_test")
    return video_recorder


def run_isolated(script, output):
    # 释放 GIL 的回归可能阻塞控制线程，使用进程超时确保测试能报告失败。
    result = subprocess.run(
        [sys.executable, "-c", textwrap.dedent(script), str(output)],
        capture_output=True, timeout=20,
    )
    assert result.returncode == 0, (result.stdout + result.stderr).decode("utf-8", errors="replace")


def test_python_thread_can_pause_resume_stop_during_native_run(native_binding, tmp_path):
    run_isolated("""
        import sys
        import threading
        import time
        from video_recorder import Recorder, RecorderError

        recorder = Recorder(sys.argv[1], 0, 0, 320, 240, fps=17,
                            system_audio=False, hardware=False, cursor=False)
        recorder._configure_test(duration=5)
        ready, paused, resumed = (threading.Event() for _ in range(3))
        events = []
        failures = []

        def callback(event, data):
            events.append(event)
            if event == 'ready':
                ready.set()
            elif event == 'paused':
                paused.set()
            elif event == 'resumed':
                resumed.set()

        def control():
            try:
                assert ready.wait(5)
                time.sleep(0.2)
                recorder.pause()
                assert paused.wait(2), 'Python control thread must run while Rust is recording'
                recorder.resume()
                assert resumed.wait(2)
                time.sleep(0.2)
                recorder.stop()
            except BaseException as error:
                failures.append(repr(error))
                recorder.cancel()

        thread = threading.Thread(target=control)
        thread.start()
        recorder.run(callback)
        thread.join(5)
        assert not thread.is_alive() and not failures, failures
        assert 'paused' in events and 'resumed' in events and 'complete' in events, events
        try:
            recorder.run(callback)
        except RecorderError as error:
            assert error.code
        else:
            raise AssertionError('a completed session cannot be reused')
    """, tmp_path / "线程控制.mp4")


def test_callback_exception_preserves_original_error_and_releases_output(native_binding, tmp_path):
    run_isolated("""
        import sys
        from pathlib import Path
        import av
        from video_recorder import Recorder

        class CallbackFailure(Exception):
            pass

        path = Path(sys.argv[1])
        recorder = Recorder(str(path), 0, 0, 320, 240, fps=17,
                            system_audio=False, hardware=False, cursor=False)
        recorder._configure_test(duration=3)
        def callback(event, data):
            if event == 'progress':
                raise CallbackFailure('callback failed')
        try:
            recorder.run(callback)
        except CallbackFailure as error:
            assert str(error) == 'callback failed'
        else:
            raise AssertionError('callback error must reach caller')
        renamed = path.with_name('released.mp4')
        path.rename(renamed)
        with av.open(str(renamed)) as container:
            assert len(list(container.decode(video=0))) >= 1
    """, tmp_path / "callback.mp4")


@pytest.mark.parametrize("options", [{"fps": 0}, {"bitrate": 499_999}, {"width": 321}])
def test_invalid_native_options_never_create_output(native_binding, tmp_path, options):
    output = tmp_path / "invalid.mp4"
    arguments = dict(output=str(output), left=0, top=0, width=320, height=240,
                     system_audio=False, hardware=False, cursor=False)
    arguments.update(options)
    with pytest.raises(native_binding.RecorderError) as error:
        recorder = native_binding.Recorder(**arguments)
        recorder._configure_test(duration=0.1)
        recorder.run(lambda *_: None)
    assert error.value.code == "arguments"
    assert not output.exists()


def test_native_existing_file_is_preserved(native_binding, tmp_path):
    output = tmp_path / "existing.mp4"
    output.write_bytes(b"existing user file")
    recorder = native_binding.Recorder(str(output), 0, 0, 320, 240,
                                      system_audio=False, hardware=False, cursor=False)
    recorder._configure_test(duration=0.1)
    with pytest.raises(native_binding.RecorderError) as error:
        recorder.run(lambda *_: None)
    assert error.value.code == "output_exists"
    assert output.read_bytes() == b"existing user file"
