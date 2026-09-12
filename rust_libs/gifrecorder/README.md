# j-gif

Screen recording to GIF for Python, implemented in Rust.

Frames are captured through Win32 GDI (BitBlt), compressed to JPEG and kept in a
native frame store, then encoded to GIF -- all on native threads, so the GIL is
never held during recording or export.

- `FrameStore` -- accepts BGRA/RGB frames, stores them JPEG-compressed, exports GIF
- `FrameDecoder` -- background decoding for playback over a bounded channel
- `RecordSession` -- recording state machine (recording / paused / stopped)

The distribution is `j-gif`; the module imports as `gifrecorder`.

```python
import gifrecorder

store = gifrecorder.FrameStore(width, height, fps, jpeg_quality=90)
store.push_bgra(bgra_bytes, elapsed_ms)      # once per captured frame
store.export_gif("out.gif", repeat=0)
```

Playback can start directly at a frame without decoding earlier JPEGs:

```python
decoder = store.start_decoder(display_w=640, display_h=360, start_frame=100)
try:
    frame = decoder.next_frame()  # (RGB bytes, original elapsed_ms), or None
finally:
    decoder.stop()
```

`start_frame` defaults to zero. An index at or beyond the end produces an empty
decoder. `total_frames` counts frames from the chosen start to the end, and
`fetched_count` starts at zero. Timestamps remain relative to the original recording.
This parameter requires rebuilding the extension from this source; the published
0.3.0 wheel does not yet provide it.

Windows x86_64 or ARM64, CPython 3.11+ (abi3). Part of
[jietuba](https://github.com/1003129155/jietuba). MIT licensed.
