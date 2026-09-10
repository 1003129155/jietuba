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

Windows x86_64, CPython 3.11+ (abi3). Part of
[jietuba](https://github.com/1003129155/jietuba). MIT licensed.
