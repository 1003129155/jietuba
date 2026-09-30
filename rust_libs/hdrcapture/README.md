# j-hdrcapture

HDR-correct Windows desktop capture for Python, implemented in Rust.

Frames come from DXGI Desktop Duplication in FP16, are normalized on the GPU to each
monitor's Windows SDR white level, and are read back as tightly packed sRGB `BGRA8`:

- content at or below SDR white is returned unchanged, matching a GDI capture pixel for pixel;
- brighter highlights are divided by their brightest channel, which brings them back to white
  while keeping their hue. 8-bit sRGB has no code values above white, so keeping SDR content
  exact leaves no room to grade highlights.

GDI `BitBlt` clips each channel separately above desktop white on an HDR monitor, which shifts
the hue of colored highlights.

```python
import hdrcapture

cap = hdrcapture.Capture(timeout_ms=100)      # keep one session; don't create one per frame
monitor = cap.monitors[0]                     # 0 = the whole virtual desktop
frame = cap.grab(monitor)
frame.bgra                                    # bytes, tightly packed BGRA8
monitor.rect                                  # (x, y, width, height), physical pixels, may be negative
region = cap.grab_region(100, 200, 640, 360)  # reads back only this region, physical pixels
```

`grab_region()` crops on the GPU when the region lies within one monitor and composes the whole
virtual desktop only when it spans monitors. An empty region, or one outside the virtual desktop,
raises `CaptureError`. `grab()` also accepts an integer index or a dict with an `index` key.

The distribution is `j-hdrcapture`; the module imports as `hdrcapture`. Without the default
`python` feature the crate is a plain Rust library.

## Adaptive tone mapping

With `grab(monitor, adaptive=True)`, a monitor on which enough pixels are more than 2 % brighter
than SDR white is darkened as a whole along the SMPTE ST 2094-50 reference-white curve, down to
half of SDR white at most, to leave room for highlight detail. The peak is the 95th percentile of
the qualifying 32×32 tile peaks, so a few very bright specks may still clip. Monitors without
enough HDR content get exactly the default static mapping. The peak that was used is reported in
`frame.monitor_info[i]["tone_map_peak"]`, or `None` when the curve was not applied.

The adaptive curve follows the content, so repeated captures of the same scene can differ. Use the
default static mapping where overlapping captures must match, such as scroll stitching or
frame-by-frame recording.

All exceptions derive from `hdrcapture.CaptureError`, itself a `RuntimeError`:
`InitialFrameTimeout`, `AccessLost`, `DimensionsChanged` and `InvalidMonitorIndex`.

## Session lifetime

A new session returns its first frame only after a real desktop present, and raises
`InitialFrameTimeout` if none arrives within the budget. Presents arrive every few milliseconds
while the display is awake and not at all while it is asleep.

Every `grab()` re-enumerates the display topology, so connecting or disconnecting a monitor,
toggling HDR, changing a resolution or position, or moving the "SDR content brightness" slider
rebuilds the session. A rebuild drops all cached frames, so the first `grab()` after such a change
starts cold; callers should be prepared for `InitialFrameTimeout`.

## Attribution

`src/d3d11.rs`, `src/dxgi_duplication_api.rs` and `src/monitor.rs` are adapted from
[windows-capture](https://github.com/NiiightmareXD/windows-capture) by NiiightmareXD
(MIT License, commit `c7d1064`), with the parts that depend on Windows.Graphics.Capture removed.
Its license is included as `LICENSE-UPSTREAM`. The HDR capture pipeline in `src/hdr_capture/` and
the Python bindings were written for this project.

Windows x86_64 or ARM64, CPython 3.11+ (abi3). Part of
[jietuba](https://github.com/1003129155/jietuba). MIT licensed.
