"""PyO3 录制子进程：只加载标准库和录制扩展，帧数据始终留在 Rust 中。"""

from __future__ import annotations

import argparse
from contextlib import ExitStack
from functools import lru_cache
import json
import os
import sys
import threading


PROTOCOL = 1
MAX_COMMAND = 2048
MAX_MESSAGE = 16 * 1024


def _standard_fd(name, number):
    """windowed onefile 没有 sys.stdout，仍可使用 QProcess 继承的标准句柄。"""
    stream = getattr(sys, name, None)
    if stream is not None:
        try:
            return os.dup(stream.fileno())
        except (OSError, ValueError):
            pass
    if os.name != "nt":
        return os.dup(number)
    import ctypes
    from ctypes import wintypes
    import msvcrt
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.GetStdHandle.argtypes = [wintypes.DWORD]
    kernel.GetStdHandle.restype = wintypes.HANDLE
    kernel.GetCurrentProcess.restype = wintypes.HANDLE
    kernel.DuplicateHandle.argtypes = [wintypes.HANDLE, wintypes.HANDLE, wintypes.HANDLE,
                                      ctypes.POINTER(wintypes.HANDLE), wintypes.DWORD,
                                      wintypes.BOOL, wintypes.DWORD]
    kernel.DuplicateHandle.restype = wintypes.BOOL
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.restype = wintypes.BOOL
    handle = kernel.GetStdHandle(-10 - number)
    current = kernel.GetCurrentProcess()
    duplicate = wintypes.HANDLE()
    if not kernel.DuplicateHandle(current, handle, current, ctypes.byref(duplicate), 0, False, 2):
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        return msvcrt.open_osfhandle(duplicate.value, os.O_BINARY | (os.O_RDONLY if number == 0 else os.O_WRONLY))
    except BaseException:
        kernel.CloseHandle(duplicate)
        raise


@lru_cache(maxsize=1)
def _peek_pipe():
    # 控制线程重复轮询时复用 API 包装，避免每次加载 DLL、创建函数对象。
    import ctypes
    from ctypes import wintypes
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    peek = kernel.PeekNamedPipe
    peek.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
                    wintypes.LPVOID, ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
    peek.restype = wintypes.BOOL
    return peek


def _read_available(fd):
    """控制管道按可用字节读取，结束录制无需等待父进程关闭 stdin。"""
    if os.name == "nt":
        import ctypes
        from ctypes import wintypes
        import msvcrt
        available = wintypes.DWORD()
        if not _peek_pipe()(msvcrt.get_osfhandle(fd), None, 0, None, ctypes.byref(available), None):
            error = ctypes.get_last_error()
            if error in (109, 232, 233):
                return b""
            raise ctypes.WinError(error)
        if not available.value:
            return None
        return os.read(fd, min(available.value, 256))
    import select
    readable, _, _ = select.select([fd], [], [], 0)
    return os.read(fd, 256) if readable else None


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Duplicate command field")
        result[key] = value
    return result


class _Controls:
    def __init__(self, fd, recorder):
        self.fd = fd
        self.recorder = recorder
        self.error = None
        self.done = threading.Event()
        self.thread = threading.Thread(target=self._read, name="video-control", daemon=True)

    def _invalid(self, message):
        self.error = message
        # 协议失败也完成已写帧的封装，但不向界面报告成功。
        self.recorder.stop()

    def _command(self, line):
        command = json.loads(line, object_pairs_hook=_unique_object)
        if (not isinstance(command, dict) or set(command) != {"protocol", "command"}
                or type(command["protocol"]) is not int or command["protocol"] != PROTOCOL
                or command["command"] not in ("pause", "resume", "stop", "cancel")):
            raise ValueError("Invalid recording command")
        getattr(self.recorder, command["command"])()
        return command["command"] in ("stop", "cancel")

    def _read(self):
        line = bytearray()
        try:
            while not self.done.is_set():
                chunk = _read_available(self.fd)
                if chunk is None:
                    self.done.wait(0.01)
                    continue
                if not chunk:
                    if line:
                        self._invalid("Incomplete command at EOF")
                    else:
                        self.recorder.stop()
                    return
                for byte in chunk:
                    if byte == 10:
                        if self._command(bytes(line)):
                            return
                        line.clear()
                    else:
                        if len(line) == MAX_COMMAND:
                            raise ValueError("Command exceeds 2048 bytes")
                        line.append(byte)
        except (OSError, ValueError, TypeError, RuntimeError) as error:
            self._invalid(str(error))

    def start(self):
        self.thread.start()

    def close(self):
        self.done.set()
        if self.thread.ident is not None:
            self.thread.join()


class _Parser(argparse.ArgumentParser):
    def error(self, message):
        raise ValueError(message)


def _arguments(argv, test_support):
    parser = _Parser(add_help=False, allow_abbrev=False)
    for name in ("output", "left", "top", "width", "height"):
        parser.add_argument("--" + name, required=True, type=str if name == "output" else int)
    parser.add_argument("--fps", type=int, default=30)
    parser.add_argument("--bitrate", type=int, default=4_000_000)
    parser.add_argument("--audio", choices=("system", "none"), default="system")
    parser.add_argument("--encoder", choices=("auto", "software"), default="auto")
    parser.add_argument("--cursor", choices=("on", "off"), default="on")
    parser.add_argument("--capture", choices=("dxgi", "gdi"), default="dxgi")
    if test_support:
        parser.add_argument("--synthetic", action="store_true")
        parser.add_argument("--synthetic-medium", action="store_true")
        parser.add_argument("--duration", type=float)
        parser.add_argument("--fail-hardware-init", action="store_true")
    return parser.parse_args(argv)


def _emit(fd, event, data):
    message = (json.dumps({"protocol": PROTOCOL, "event": event, "data": data},
                          ensure_ascii=False, allow_nan=False, separators=(",", ":")) + "\n").encode("utf-8")
    if len(message) > MAX_MESSAGE:
        raise ValueError("Recording event exceeds 16384 bytes")
    while message:
        written = os.write(fd, message)
        if written <= 0:
            raise OSError("Recording status pipe is closed")
        message = message[written:]


def run(argv, input_fd, output_fd):
    controls = None
    try:
        import video_recorder
        test_support = hasattr(video_recorder.Recorder, "_configure_test")
        args = _arguments(argv, test_support)
        if os.name == "nt":
            import ctypes
            # 与主程序使用相同的物理像素坐标；子进程不加载 Qt。
            ctypes.windll.user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
        recorder = video_recorder.Recorder(
            args.output, args.left, args.top, args.width, args.height,
            fps=args.fps, bitrate=args.bitrate, system_audio=args.audio == "system",
            hardware=args.encoder == "auto", cursor=args.cursor == "on",
            prefer_dxgi=args.capture == "dxgi",
        )
        if test_support:
            recorder._configure_test(synthetic=args.synthetic or args.synthetic_medium,
                                     medium=args.synthetic_medium, duration=args.duration,
                                     fail_hardware=args.fail_hardware_init)
        controls = _Controls(input_fd, recorder)

        def event(name, data):
            if controls.error is None or name not in ("complete", "cancelled"):
                _emit(output_fd, name, data)

        controls.start()
        recorder.run(event)
        controls.close()
        if controls.error is not None:
            _emit(output_fd, "error", {"code": "protocol", "message": controls.error[:1024]})
            return 1
        return 0
    except Exception as error:
        code = "protocol" if controls is not None and controls.error is not None else getattr(error, "code", None)
        if code is None:
            code = ("helper_missing" if isinstance(error, ImportError)
                    else "arguments" if isinstance(error, (ValueError, OverflowError, TypeError))
                    else "backend")
        try:
            _emit(output_fd, "error", {"code": str(code), "message": str(error)[:1024]})
        except (OSError, ValueError):
            pass
        return 1
    finally:
        if controls is not None:
            controls.close()


def main(argv=None):
    try:
        with ExitStack() as stack:
            output = _standard_fd("stdout", 1)
            stack.callback(os.close, output)
            try:
                source = _standard_fd("stdin", 0)
            except OSError as error:
                _emit(output, "error", {"code": "process_io", "message": str(error)[:1024]})
                return 1
            stack.callback(os.close, source)
            return run(sys.argv[1:] if argv is None else argv, source, output)
    except OSError:
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
