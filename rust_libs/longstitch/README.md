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

Windows x86_64, CPython 3.11+ (abi3). Part of
[jietuba](https://github.com/1003129155/jietuba). MIT licensed.
