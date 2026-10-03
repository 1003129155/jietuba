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
result has grown. It matches frames the way `stitch()` does, but looks for each
frame near the previous one, so scrolling back is tracked instead of trimming
the result, and the result can grow at either end.

```python
session = longstitch.StitchSession(width, height)
for bgra, heading in frames:             # bytes, width * height * 4; "down", "up" or None
    step = session.push(bgra, heading=heading)
    if step is None:
        continue                         # no overlap; the session is unchanged
    # step.top is the row of the result where this frame starts
image = session.export()                 # BGRA bytes, width x session.height
```

A frame that lands inside what has already been stitched (the user scrolled
back) leaves the result unchanged and only moves `step.top`. A frame that runs
past the end extends it: the result was cut to `step.keep` rows, then frame
rows from `step.skip` on were appended. A frame that runs past the start
extends the top: `step.head_cut` rows were removed from the top, then the first
`step.head_rows` frame rows were put in front. So a capture can start in the
middle of a page and grow both ways. Rows already stitched are only replaced
where the result's edge disagrees with the frame (an old fixed header or
footer); a small difference inside the overlap, such as a hover highlight, is
left out.

`heading` is the direction scrolled before this frame, `"down"` meaning towards
higher result rows. It decides between places that match equally well; with
`None` the session follows the direction of the last movement, exposed as
`session.last_move`. If nothing
credible matches near the previous frame, the whole result is searched, so a
jump such as Home/End is still located.

Rows are first compared pixel for pixel (ignoring at least a tenth of the width
on the right, where a scrollbar may sit), so list entries that differ in a few
characters are told apart; only if that finds nothing are rows compared by
average colour, which tolerates slight rendering differences. A place counts
when it is backed by rows that occur once in both the frame and the result;
failing that, the match has to cover most of what would overlap and either
contain a couple of such rows or be long (entries that repeat exactly, a large
blank area). A frame that merely shares a few rows with the result
(near-identical entries, blank lines between paragraphs, its fixed header or
footer) is reported as no overlap instead of being forced in; scroll back until
the view overlaps what has been stitched and it joins again.

`session.crop_top()` drops everything above the latest frame and
`session.crop_bottom()` everything below it; both return the number of rows
removed, and later frames keep growing the result as before. Scroll back to
where the capture should start or end, then crop.

Windows x86_64 or ARM64, CPython 3.11+ (abi3). Part of
[jietuba](https://github.com/1003129155/jietuba). MIT licensed.
