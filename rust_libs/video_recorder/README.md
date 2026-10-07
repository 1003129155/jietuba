# j-video

Windows screen-region recording to MP4 for Python, implemented in Rust.

The extension captures a rectangle of the desktop, encodes it as H.264 through Windows Media
Foundation and can add the system's playback audio as AAC. Capture, color conversion, encoding and
audio buffering all run in Rust, so no raw frames or audio blocks ever cross into Python. FFmpeg,
Qt Multimedia and extra codec DLLs are neither needed nor bundled.

```python
import video_recorder

recorder = video_recorder.Recorder(
    r"C:\Videos\clip.mp4",         # absolute path of a new .mp4 file
    0, 0, 1920, 1080,              # left, top, width, height in physical pixels; width and height even
    fps=30, bitrate=4_000_000,     # 1-60 fps, 500_000-50_000_000 bit/s (these are the defaults)
    system_audio=True, hardware=True, cursor=True, prefer_dxgi=True,   # the defaults
)

def on_event(name, data):          # "ready", "progress", "paused", "resumed", "complete", "cancelled"
    print(name, data)

recorder.run(on_event)             # records on this thread, with the GIL released, until stopped

# from another thread, while run() is active:
recorder.pause()
recorder.resume()
recorder.stop()                    # finish the MP4; "complete" follows once the file is closed
recorder.cancel()                  # stop and delete the file; "cancelled" follows
```

A `Recorder` runs once. `run()` takes the GIL only to call `on_event`, so any thread may call
`pause()`, `resume()`, `stop()` or `cancel()` at any time.

`run()` initializes multithreaded COM and Media Foundation on the calling thread and releases them
when it returns. Call it from a plain worker thread: on a thread that is already in a single-threaded
COM apartment, such as a Qt GUI thread, it fails right away with a `RecorderError`.

## Arguments

- `width` and `height` must be even, at least 2 and at most 8192, with at most 33,177,600 pixels (8K).
- The rectangle must lie inside the virtual desktop; negative coordinates are fine on multi-monitor
  setups. The recording thread runs per-monitor DPI aware, so all coordinates are physical pixels.
- `output` must be an absolute path ending in `.mp4`. The file is created atomically and an existing
  file is never overwritten (`RecorderError` with code `output_exists`).
- `hardware=True` asks for a hardware H.264 encoder and falls back to the system software encoder if
  the writer cannot be initialized. Windows can pick the Microsoft DX12 encoder for small frames; it
  fails while writing samples, so it counts as unusable and the software encoder is used instead. A
  failure after recording has started is reported, never silently downgraded and never turned into
  silence.
- `bitrate` is a constant-bitrate target, not a file size cap; busy content can exceed it. A lower
  `fps` reduces capture and encoding work but does not shrink the file in proportion.
- `cursor=True` draws the mouse pointer by its hotspot. `prefer_dxgi=False` forces GDI capture.

## Events

`on_event(name, data)` receives a `str` and a `dict`:

| name | data |
|---|---|
| `ready` | `encoder`, `hardware`, `encoder_identified`, `width`, `height`, `fps`, `bitrate`, `audio` (`"system"` or `"none"`) |
| `progress` | about once per second of recording: `frames`, `elapsed_ms`, `dropped`, `queued_bytes`, `max_queued_bytes` |
| `paused`, `resumed`, `cancelled` | `frames`, `elapsed_ms`, `dropped` |
| `complete` | `frames`, `elapsed_ms`, `dropped`, `output` (also as `path`), `encoder`, `hardware`, `finalized`, `max_queued_bytes` |

`elapsed_ms` leaves out paused time. `dropped` counts capture slots skipped because the encoder was
behind, and `queued_bytes` is the sink writer's input queue, not the total memory used by the encoder
and its driver. `hardware` is whether the encoder was identified as a hardware one, and
`encoder_identified` tells whether it could be identified at all. `complete` is sent only after the
MP4 has been finalized and the file released. `stop()` before the first frame ends with `cancelled`
and leaves no file.

## Errors

Failures raise `video_recorder.RecorderError`, a `RuntimeError` whose `code` attribute is stable (for
example `arguments`, `output_exists`, `capture`, `encoding`, `audio_device` or `audio_format`); the
message is for diagnostics only. An exception raised by `on_event` propagates out of `run()`
unchanged, after the recorder has finalized the MP4 as far as it can. After any failure the file may
hold a partial recording, which must not be treated as a complete one.

## Audio

`system_audio=True` records the default playback device through WASAPI loopback and encodes it as
AAC. The device can have 1 to 8 channels in float32 or 16/24/32-bit PCM; audio is resampled to
48 kHz and multi-channel layouts are down-mixed to stereo by speaker mask, keeping the center,
surround and low-frequency channels. Stretches without audio are filled with silence. A missing
device or an unsupported format raises an error instead of dropping the audio. Pausing pauses the
audio too, so paused time is not in the file. There is no microphone capture, no mixing, and no
support for switching the playback device during a recording.

## Video

Frames are SDR sRGB converted to BT.601 limited-range NV12; there is no 10-bit or HDR video. GDI
clips everything above desktop white on an HDR monitor, so with `prefer_dxgi=True` frames come from
DXGI Desktop Duplication and are tone-mapped to sRGB on the GPU, using the same pipeline as
[j-hdrcapture](https://pypi.org/project/j-hdrcapture/); only the recorded region is read back. GDI
is used when DXGI is unavailable or has not delivered a frame yet, and both paths produce identical
pixels for SDR content. Protected windows, desktop switches and the secure desktop cannot be recorded.

One BGRA capture surface is reused and converted straight into the encoder's NV12 buffers, and no
frame history is kept: the sink writer applies back-pressure and late capture slots are dropped
(counted in `dropped`). The system and its drivers keep their own encoder buffers.

## Requirements

Windows 10 or later, with the system's Media Foundation H.264 and AAC encoders. Windows N and KN
editions need the Media Feature Pack; if initialization fails an error is raised and nothing is
downloaded.

The distribution is `j-video`; the module imports as `video_recorder`.

## Credits

Originally contributed to jietuba by [LowValueTarget777](https://github.com/LowValueTarget777) in
[pull request #70](https://github.com/1003129155/jietuba/pull/70).

Windows x86_64 or ARM64, CPython 3.11+ (abi3). Part of
[jietuba](https://github.com/1003129155/jietuba). MIT licensed.
