# j-clipboard

Windows clipboard access and history management for Python, implemented in Rust.

Reads and writes text, images, HTML, RTF and file lists, and keeps a searchable
history in SQLite with grouping, pagination and paste counts. Payloads over
100 KB are zstd-compressed.

The distribution is `j-clipboard`; the module imports as `pyclipboard`.

```python
import pyclipboard

pyclipboard.set_clipboard_text("hello")
print(pyclipboard.get_clipboard_text())

manager = pyclipboard.ClipboardManager()      # defaults to a per-user data dir
manager.add_item("hello")
for item in manager.get_history():
    print(item.content)
```

Windows x86_64 or ARM64, CPython 3.11+ (abi3). Part of
[jietuba](https://github.com/1003129155/jietuba). MIT licensed.
