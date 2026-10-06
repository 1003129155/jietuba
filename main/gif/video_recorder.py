# -*- coding: utf-8 -*-
"""独立 MP4 录制进程控制器：只传递命令与状态，不持有视频帧。"""

from __future__ import annotations

import json
import math
from pathlib import Path

from PySide6.QtCore import QObject, QProcess, QProcessEnvironment, QRect, QTimer, Signal
from capture.capture_service import uses_hdr_engine
from core.i18n import make_tr
from video_helper import worker_command as default_worker_command

_tr = make_tr("VideoRecorder")


PROTOCOL = 1
MAX_MESSAGE = 16 * 1024
MAX_STDERR = 16 * 1024


def failure_message(code, detail):
    """面向用户的处理建议；原始错误仍通过 failed 信号写入日志。"""
    messages = {
        "helper_missing": "录制组件不存在，请重新安装或从当前源码构建。",
        "output_exists": "目标文件已存在，请选择新的文件名。",
        "protocol": "录制进程返回了无效的状态消息。",
        "audio_device": "找不到可用的播放设备，请连接音箱或耳机，或在视频设置中关闭系统声音后重试。",
        "audio_format": "当前播放设备的音频格式不受支持，请在 Windows 声音设置中选择立体声格式，或关闭系统声音后重试。",
        "audio": "系统声音录制失败，请检查播放设备，或在视频设置中关闭系统声音后重试。",
        "audio_timestamp": "播放设备的声音时间戳异常，录制已停止。请重新连接设备，或关闭系统声音后重试。",
        "encoding": "视频编码失败，请尝试降低分辨率或帧率，或在视频设置中关闭硬件编码后重试。",
        "capture": "无法采集录制区域，请检查屏幕连接和选区后重试。",
        "output": "无法创建视频文件，请检查保存位置的写入权限和剩余空间。",
        "disk_full": "保存位置空间不足，请释放空间或选择其他磁盘后重试。",
        "arguments": "录制参数或区域无效，请重新调整选区和视频设置。",
    }
    if code in messages:
        return _tr(messages[code])
    if code in {
        "invalid_region", "process_io", "cancelled",
        "start_failed", "process_crashed", "start_timeout", "finalize_timeout", "command_timeout",
        "incomplete", "release_failed", "release_timeout",
    }:
        return detail
    return _tr("视频录制失败，请重试。若问题持续，请查看日志中的详细错误。")


class VideoRecorder(QObject):
    ready = Signal()
    progress = Signal(float)
    paused_changed = Signal(bool)
    completed = Signal(str)
    failed = Signal(str, str)
    finished = Signal()

    START_TIMEOUT = 30_000
    STOP_TIMEOUT = 30_000
    COMMAND_TIMEOUT = 5_000

    def __init__(self, parent=None, *, process_factory=QProcess, worker_command=None):
        super().__init__(parent)
        # 注入点供正式自动化测试使用；界面不接受运行时替换组件的环境变量。
        self._process_factory = process_factory
        self._worker_command = list(worker_command) if worker_command else None
        self._process = None
        self._state = "idle"
        self._stdout = bytearray()
        self._stderr = bytearray()
        self._output = None
        self._complete = None
        self._failure = None
        self._timer = QTimer(self)
        self._timer.setSingleShot(True)
        self._timer.timeout.connect(self._on_timeout)
        self._kill_timer = QTimer(self)
        self._kill_timer.setSingleShot(True)
        self._kill_timer.timeout.connect(self._kill_unresponsive)

    @property
    def active(self):
        return self._state != "idle"

    @property
    def state(self):
        return self._state

    @property
    def stderr_tail(self):
        return bytes(self._stderr).decode("utf-8", errors="replace")

    def start(self, rect: QRect, output: str, options):
        if self.active:
            return False
        try:
            command = self._worker_command or default_worker_command()
        except (ImportError, OSError, ValueError):
            command = None
        target = Path(output).absolute()
        if not command or not Path(command[0]).is_file():
            self.failed.emit("helper_missing", _tr("录制组件不存在，请重新安装或从当前源码构建。"))
            return False
        if target.exists():
            self.failed.emit("output_exists", _tr("目标文件已存在，请选择新的文件名。"))
            return False
        # 与 GIF 一样内缩边框，并只裁掉末尾一个奇数像素，避免蓝/红边框入镜。
        width = (rect.width() - 2) & ~1
        height = (rect.height() - 2) & ~1
        if width < 2 or height < 2:
            self.failed.emit("invalid_region", _tr("录制区域太小，请重新调整。"))
            return False
        self._stdout.clear()
        self._stderr.clear()
        self._complete = self._failure = None
        self._output = target
        self._state = "starting"
        process = self._process_factory(self)
        self._process = process
        process.setProcessChannelMode(QProcess.ProcessChannelMode.SeparateChannels)
        environment = QProcessEnvironment.systemEnvironment()
        # 子进程与主程序共生命周期，沿用 onefile 解压目录，避免每次录制重复解包。
        process.setProcessEnvironment(environment)
        process.setWorkingDirectory(str(target.parent))
        process.readyReadStandardOutput.connect(self._read_stdout)
        process.readyReadStandardError.connect(self._read_stderr)
        process.errorOccurred.connect(self._on_process_error)
        process.finished.connect(self._on_finished)
        args = ["--output", str(target), "--left", str(rect.x() + 1),
                "--top", str(rect.y() + 1), "--width", str(width),
                "--height", str(height), "--fps", str(options.fps),
                "--bitrate", str(options.bitrate),
                "--audio", "system" if options.system_audio else "none",
                "--encoder", "auto" if options.hardware else "software",
                "--cursor", "on" if options.cursor else "off",
                # 与截图、GIF 同一个引擎设置；子进程不加载 hdrcapture，由这里决定。
                "--capture", "dxgi" if uses_hdr_engine() else "gdi"]
        self._timer.start(self.START_TIMEOUT)
        process.start(command[0], [*command[1:], *args])
        return True

    def _send(self, command):
        if self._process is None:
            return False
        line = json.dumps({"protocol": PROTOCOL, "command": command}, separators=(",", ":")) + "\n"
        return self._process.write(line.encode("utf-8")) >= 0

    def pause(self, paused):
        if (paused and self._state != "recording") or (not paused and self._state != "paused"):
            return False
        self._state = "pausing" if paused else "resuming"
        self._timer.start(self.COMMAND_TIMEOUT)
        if not self._send("pause" if paused else "resume"):
            self._fail("process_io", _tr("无法发送录制命令。"))
        return True

    def stop(self):
        if not self.active or self._state == "stopping":
            return
        self._state = "stopping"
        self._timer.start(self.STOP_TIMEOUT)
        if not self._send("stop"):
            self._fail("process_io", _tr("无法发送停止命令。"))
        else:
            # 停止后不再发送控制命令；Qt 先排空已写数据再关管道，使 helper 读线程获得 EOF。
            self._process.closeWriteChannel()

    def _read_stdout(self):
        if self._process is None:
            return
        self._stdout.extend(bytes(self._process.readAllStandardOutput()))
        while b"\n" in self._stdout:
            end = self._stdout.index(b"\n")
            if end > MAX_MESSAGE:
                self._stdout.clear()
                self._fail("protocol", _tr("录制进程返回了过大的状态消息。"))
                return
            line = bytes(self._stdout[:end]).rstrip(b"\r")
            del self._stdout[:end + 1]
            try:
                message = json.loads(line)
                self._handle_message(message)
            except (ValueError, TypeError, KeyError, OverflowError):
                self._fail("protocol", _tr("录制进程返回了无效的状态消息。"))
                return
        if len(self._stdout) > MAX_MESSAGE:
            self._stdout.clear()
            self._fail("protocol", _tr("录制进程返回了过大的状态消息。"))

    def _read_stderr(self):
        if self._process is not None:
            self._stderr.extend(bytes(self._process.readAllStandardError()))
            del self._stderr[:-MAX_STDERR]

    def _handle_message(self, message):
        if not isinstance(message, dict) or message.get("protocol") != PROTOCOL:
            raise ValueError("protocol")
        data = message.get("data", {})
        if not isinstance(data, dict):
            raise ValueError("data")
        event = message["event"]
        if self._failure is not None:
            return
        if event == "ready":
            if self._state == "starting":
                self._timer.stop()
                self._state = "recording"
                self.ready.emit()
            elif self._state != "stopping":
                raise ValueError("unexpected ready")
        elif event == "progress":
            elapsed = data["elapsed_ms"]
            if (isinstance(elapsed, bool) or not isinstance(elapsed, (int, float))
                    or not math.isfinite(elapsed) or elapsed < 0):
                raise ValueError("elapsed")
            if self._state in ("recording", "paused", "pausing", "resuming"):
                self.progress.emit(elapsed / 1000)
        elif event in ("paused", "resumed"):
            paused = event == "paused"
            expected = "pausing" if paused else "resuming"
            if self._state == expected:
                elapsed = data.get("elapsed_ms")
                if (isinstance(elapsed, bool) or not isinstance(elapsed, (int, float))
                        or not math.isfinite(elapsed) or elapsed < 0):
                    raise ValueError("elapsed")
                self._timer.stop()
                self._state = "paused" if paused else "recording"
                self.paused_changed.emit(paused)
                self.progress.emit(elapsed / 1000)
            elif self._state != "stopping":
                raise ValueError("unexpected ack")
        elif event == "complete":
            result_path = Path(data["output"]).absolute()
            if data.get("finalized") is not True or result_path != self._output:
                raise ValueError("incomplete output")
            self._complete = str(result_path)
            self._state = "stopping"
            self._timer.start(self.STOP_TIMEOUT)
        elif event == "error":
            self._fail(str(data.get("code", "backend")), str(data.get("message", _tr("录制失败。"))))
        elif event == "cancelled":
            self._fail("cancelled", _tr("录制已取消，未保存视频。"))
        else:
            raise ValueError("unknown event")

    def _fail(self, code, message):
        if self._failure is None:
            self._failure = (code, message)
        self._timer.stop()
        self._state = "stopping"
        if self._process is not None:
            self._send("cancel")
            self._process.closeWriteChannel()
            self._kill_timer.start(2_000)

    def _on_process_error(self, error):
        if error == QProcess.ProcessError.FailedToStart:
            self._failure = ("start_failed", _tr("无法启动录制进程。"))
            self._finish(False)
        elif error == QProcess.ProcessError.Crashed:
            self._fail("process_crashed", _tr("录制进程意外退出，视频可能未完成。"))

    def _on_timeout(self):
        if self._state in ("pausing", "resuming"):
            self._fail("command_timeout", _tr("暂停或恢复录制超时，录制已停止，视频未确认保存成功。"))
        else:
            code = "start_timeout" if self._state == "starting" else "finalize_timeout"
            self._fail(code, _tr("录制进程响应超时，视频未确认保存成功。"))

    def _kill_unresponsive(self):
        if self._process is not None:
            # 只清理本控制器创建的 helper，不影响其他程序；失败视频不会报告成功。
            self._process.kill()

    def _on_finished(self, exit_code, exit_status):
        self._read_stdout()
        self._read_stderr()
        success = exit_code == 0 and exit_status == QProcess.ExitStatus.NormalExit
        self._finish(success)

    def _finish(self, successful_exit):
        if self._process is None:
            return
        self._timer.stop()
        self._kill_timer.stop()
        success = successful_exit and self._complete and self._failure is None
        if success:
            try:
                success = self._output.is_file() and self._output.stat().st_size > 0
            except OSError:
                success = False
        failure = self._failure or ("incomplete", _tr("录制未正常完成，未确认视频保存成功。"))
        process, self._process = self._process, None
        process.deleteLater()
        self._state = "idle"
        if success:
            self.completed.emit(self._complete)
        else:
            self.failed.emit(*failure)
        self.finished.emit()

    def shutdown(self):
        """应用退出时有界等待封装结束，确保 onefile 清理前不存在遗留 helper。"""
        if not self.active or self._process is None:
            return
        self.stop()
        process = self._process
        if not process.waitForFinished(self.STOP_TIMEOUT):
            self._failure = ("finalize_timeout", _tr("退出时录制未能完成。"))
            process.kill()
            process.waitForFinished(2_000)
