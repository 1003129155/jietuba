# j-stitch

Scrolling screenshot stitching for Python, implemented in Rust.

Finds the vertical overlap between two consecutive screenshots by hashing each
pixel row and running a longest-common-substring match over the hash sequences,
then splices the images. Optionally detects whether the second image belongs
above or below the first.

The distribution is `j-stitch`; the module imports as `longstitch`.

```python
import longstitch

result = longstitch.stitch(png_bytes_1, png_bytes_2)
if result is None:
    print("the two images do not overlap")
else:
    open("stitched.png", "wb").write(result.png)
```

`stitch()` returns `None` when no overlap is found -- that is a legitimate
outcome, not a failure. Decode and encode failures raise `longstitch.StitchError`.

## Incremental sessions

For a long capture, `StitchSession` keeps the stitched image inside Rust and
takes raw BGRA frames, so each frame costs the same no matter how long the
result has grown. It applies the same matching rules as `stitch()`.

```python
session = longstitch.StitchSession(width, height)
for bgra in frames:                      # bytes, width * height * 4
    step = session.push(bgra)
    if step is None:
        continue                         # no overlap; the session is unchanged
    # the result was cut to step.keep rows, then frame rows from step.skip on were appended
image = session.export()                 # BGRA bytes, width x session.height
```

`push(..., detect_direction=True)` reports `direction == "reverse"` when the
frame belongs above the result; the session then holds the flipped result, and
later frames must be flipped before they are pushed.

Windows x86_64 or ARM64, CPython 3.11+ (abi3). Part of
[jietuba](https://github.com/1003129155/jietuba). MIT licensed.
