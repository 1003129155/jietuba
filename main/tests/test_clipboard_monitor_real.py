# -*- coding: utf-8 -*-
"""剪贴板监听在真实系统剪贴板上的行为。

会改写系统剪贴板，默认跳过；设 RUN_REAL_CLIPBOARD_TESTS=1 才运行，运行期间不要复制东西。
"""
import ctypes
import os
import struct
import subprocess
import sys
import time

import pytest

pytestmark = pytest.mark.skipif(
    os.environ.get("RUN_REAL_CLIPBOARD_TESTS") != "1",
    reason="会改写系统剪贴板；设 RUN_REAL_CLIPBOARD_TESTS=1 才运行",
)

CF_UNICODETEXT = 13
FIRST_REGISTERED_FORMAT = 0xC000


def _register(name):
    return ctypes.windll.user32.RegisterClipboardFormatW(name)


def _open(attempts=50):
    import win32clipboard as cb
    for _ in range(attempts):
        try:
            cb.OpenClipboard()
            return True
        except Exception:
            time.sleep(0.01)
    return False


def _utf16(text):
    # SetClipboardData 收到纯数字字符串会当成内存句柄，文本一律按 UTF-16 字节传
    return text.encode("utf-16-le") + b"\0\0"


def _write(formats):
    """formats: [(格式编号, 数据)]，清空后一次写入；文本可以直接给 str"""
    import win32clipboard as cb
    assert _open()
    try:
        cb.EmptyClipboard()
        for fmt, data in formats:
            cb.SetClipboardData(fmt, _utf16(data) if isinstance(data, str) else data)
    finally:
        cb.CloseClipboard()


def _read(fmt):
    import win32clipboard as cb
    assert _open()
    try:
        return cb.GetClipboardData(fmt) if cb.IsClipboardFormatAvailable(fmt) else None
    finally:
        cb.CloseClipboard()


def _wait_until(condition, timeout=3.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if condition():
            return True
        time.sleep(0.05)
    return condition()


@pytest.fixture
def monitor(tmp_path):
    pyclipboard = pytest.importorskip("pyclipboard")
    manager = pyclipboard.ClipboardManager(db_path=str(tmp_path / "clipboard.db"))
    records = []
    manager.start_monitor(callback=records.append)
    time.sleep(0.3)
    yield manager, records
    manager.stop_monitor()


def test_a_copy_is_recorded(monitor):
    manager, records = monitor
    _write([(CF_UNICODETEXT, "第一次复制")])
    assert _wait_until(lambda: len(records) == 1)
    assert records[0].content == "第一次复制"


def test_a_burst_of_changes_is_recorded_once(monitor):
    _, records = monitor
    for i in range(5):
        _write([(CF_UNICODETEXT, f"连续写入 {i}")])
        time.sleep(0.02)
    assert _wait_until(lambda: records)
    time.sleep(0.5)
    assert [r.content for r in records] == ["连续写入 4"]


def test_a_writer_that_clears_then_writes_is_not_locked_out(monitor):
    """先清空、紧接着再打开写内容（表格程序复制就是这样）：第二次打开不能被监听占住。"""
    import win32clipboard as cb
    user32 = ctypes.windll.user32
    user32.OpenClipboard.argtypes = [ctypes.c_void_p]
    _, records = monitor
    locked_out = 0
    for i in range(30):
        _write([])
        if not user32.OpenClipboard(None):
            locked_out += 1
            continue
        try:
            cb.EmptyClipboard()
            cb.SetClipboardData(CF_UNICODETEXT, _utf16(f"第 {i} 次"))
        finally:
            cb.CloseClipboard()
        time.sleep(0.25)
    assert locked_out == 0
    assert records and records[-1].content == "第 29 次"


def test_pasting_from_history_is_not_recorded_again(monitor):
    manager, records = monitor
    _write([(CF_UNICODETEXT, "历史里的一条")])
    assert _wait_until(lambda: len(records) == 1)
    assert manager.paste_item(records[0].id, True, False)
    time.sleep(0.6)
    assert len(records) == 1
    # 跳过只针对写回去的那份内容，之后真正的复制照常记录
    _write([(CF_UNICODETEXT, "之后的复制")])
    assert _wait_until(lambda: len(records) == 2)


def test_pasting_a_missing_item_does_not_swallow_the_next_copy(monitor):
    manager, records = monitor
    assert manager.paste_item(987654, True, False) is False
    _write([(CF_UNICODETEXT, "粘贴失败之后的复制")])
    assert _wait_until(lambda: len(records) == 1)


@pytest.mark.parametrize("name, data", [
    ("ExcludeClipboardContentFromMonitorProcessing", b"\0"),
    ("Clipboard Viewer Ignore", b"\0"),
    ("CanIncludeInClipboardHistory", struct.pack("<I", 0)),
])
def test_content_marked_private_is_not_recorded(monitor, name, data):
    manager, records = monitor
    _write([(CF_UNICODETEXT, "不该进历史的内容"), (_register(name), data)])
    time.sleep(0.6)
    assert records == []
    assert manager.get_count() == 0


def test_content_allowed_into_history_is_recorded(monitor):
    _, records = monitor
    _write([(CF_UNICODETEXT, "允许进历史"), (_register("CanIncludeInClipboardHistory"), struct.pack("<I", 1))])
    assert _wait_until(lambda: len(records) == 1)


def test_spreadsheet_formulas_are_kept_and_pasted_under_the_current_format_id(monitor):
    """XML Spreadsheet 里有公式；注册格式的编号每次开机重新分配，旧条目要按名字写回。"""
    manager, records = monitor
    fmt = _register("XML Spreadsheet")
    xml = b'<?xml version="1.0"?><Workbook><Cell ss:Formula="=A1*2"/></Workbook>\0'
    _write([(CF_UNICODETEXT, "20"), (fmt, xml)])
    assert _wait_until(lambda: len(records) == 1)
    saved = manager.get_raw_formats(records[0].id)
    assert "XML Spreadsheet" in [name for _, name, _ in saved]

    # 模拟开机前存下的条目：注册格式的编号都对不上了
    stale = manager.add_item("20")
    manager.insert_formats(stale, [(fid + 7 if fid >= FIRST_REGISTERED_FORMAT else fid, name, bytes(data))
                                   for fid, name, data in saved])
    assert manager.paste_item(stale, True, False)
    restored = _read(fmt)
    assert restored is not None and restored.startswith(xml)
    assert _read(fmt + 7) is None


def test_monitor_waits_instead_of_taking_over_a_clipboard_another_program_holds(monitor):
    """别的程序用空窗口句柄打开剪贴板并占着时，监听不能把它关掉，要等它用完再读。"""
    import win32clipboard as cb
    user32 = ctypes.windll.user32
    user32.OpenClipboard.argtypes = [ctypes.c_void_p]
    _, records = monitor
    _write([(CF_UNICODETEXT, "触发监听")])
    time.sleep(0.05)
    assert user32.OpenClipboard(None)
    try:
        time.sleep(0.3)  # 监听在这段时间里去读剪贴板
        cb.EmptyClipboard()
        cb.SetClipboardData(CF_UNICODETEXT, _utf16("占着时写的内容"))
    finally:
        cb.CloseClipboard()
    assert _wait_until(lambda: records and records[-1].content == "占着时写的内容")


def test_pasted_content_stays_on_the_clipboard_for_other_programs(monitor):
    manager, records = monitor
    _write([(CF_UNICODETEXT, "粘贴给别的程序")])
    assert _wait_until(lambda: len(records) == 1)
    _write([(CF_UNICODETEXT, "其他内容")])
    assert manager.paste_item(records[0].id, True, False)
    reader = subprocess.run(
        [sys.executable, "-c",
         "import win32clipboard as cb; cb.OpenClipboard(); "
         "print(cb.GetClipboardData(13).encode('unicode_escape').decode()); cb.CloseClipboard()"],
        capture_output=True, text=True, timeout=30)
    assert reader.stdout.strip().encode().decode("unicode_escape") == "粘贴给别的程序"


def _png(width, height, color):
    import io
    from PIL import Image
    buf = io.BytesIO()
    Image.new("RGBA", (width, height), color).save(buf, "PNG")
    return buf.getvalue()


def _dibv5(width, height, color):
    """和截图写剪贴板的格式一样：BITMAPV5HEADER 后直接是像素，头后不再重复掩码"""
    r, g, b, a = color
    header = struct.pack("<IiiHHII4I4I", 124, width, height, 1, 32, 3, width * height * 4, 0, 0, 0, 0,
                         0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)
    header += struct.pack("<I", 0x73524742) + bytes(48) + struct.pack("<I", 4) + bytes(12)
    return header + bytes([b, g, r, a]) * (width * height)


@pytest.mark.parametrize("with_png", [True, False])
def test_an_image_copy_is_recorded_as_an_image(monitor, with_png):
    _, records = monitor
    formats = [(17, _dibv5(30, 20, (200, 100, 50, 255)))]
    if with_png:
        formats.append((_register("PNG"), _png(30, 20, (200, 100, 50, 255))))
    _write(formats)
    assert _wait_until(lambda: len(records) == 1)
    assert (records[0].content_type, records[0].content) == ("image", "[30x20]")


def test_a_file_copy_is_recorded_as_a_file_list(monitor, tmp_path):
    import json
    _, records = monitor
    files = [str(tmp_path / "截图 1.png"), str(tmp_path / "b.txt")]
    payload = struct.pack("<IiiII", 20, 0, 0, 0, 1) + "".join(f + "\0" for f in files).encode("utf-16-le") + b"\0\0"
    _write([(15, payload)])
    assert _wait_until(lambda: len(records) == 1)
    assert records[0].content_type == "file"
    assert json.loads(records[0].content)["files"] == files


def test_spreadsheet_cells_are_recorded_without_bitmaps_or_rtf(monitor):
    """单元格位图和 RTF 要表格程序现场生成，很慢；有 XML Spreadsheet 和 HTML 就够粘贴回去"""
    manager, records = monitor
    _write([
        (CF_UNICODETEXT, "10\t20"),
        (_register("XML Spreadsheet"), b"<Workbook/>\0"),
        (_register("HTML Format"), b"Version:0.9\r\n<html><body>10</body></html>\0"),
        (_register("Rich Text Format"), b"{\\rtf1 10}\0"),
        (17, _dibv5(4, 4, (0, 0, 0, 255))),
    ])
    assert _wait_until(lambda: len(records) == 1)
    names = {name for _, name, _ in manager.get_raw_formats(records[0].id)}
    assert {"XML Spreadsheet", "HTML Format", "CF_UNICODETEXT"} <= names
    assert not names & {"CF_DIB", "CF_DIBV5", "Rich Text Format", "PNG"}


def test_a_manually_added_item_pastes_as_text(monitor):
    manager, records = monitor
    item_id = manager.add_item("手动添加的 123")
    assert manager.paste_item(item_id, True, False)
    assert _read(CF_UNICODETEXT) == "手动添加的 123"
    time.sleep(0.5)
    assert records == []


def test_set_clipboard_text_handles_digits_only_text():
    import pyclipboard
    pyclipboard.set_clipboard_text("2026")
    assert _read(CF_UNICODETEXT) == "2026"


def _cf_html(fragment):
    doc = f"<html><body><!--StartFragment-->{fragment}<!--EndFragment--></body></html>".encode()
    head = "Version:0.9\r\nStartHTML:{:010}\r\nEndHTML:{:010}\r\nStartFragment:{:010}\r\nEndFragment:{:010}\r\n"
    size = len(head.format(0, 0, 0, 0))
    start = size + doc.index(b"<!--StartFragment-->") + len(b"<!--StartFragment-->")
    end = size + doc.index(b"<!--EndFragment-->")
    return head.format(size, size + len(doc), start, end).encode() + doc + b"\0"


def _fragment(cf_html):
    data = cf_html.decode("utf-8")
    fields = dict(line.split(":", 1) for line in data.splitlines()[:5])
    raw = cf_html
    return raw[int(fields["StartFragment"]):int(fields["EndFragment"])].decode("utf-8")


def test_merged_paste_keeps_every_item_in_every_format(monitor, tmp_path):
    """有一条带 HTML、RTF，其余条目也要转进去，贴到只认富文本的地方不能少"""
    manager, records = monitor
    html, rtf = _register("HTML Format"), _register("Rich Text Format")
    rich = manager.add_item("加粗的一条")
    manager.insert_formats(rich, [
        (CF_UNICODETEXT, "CF_UNICODETEXT", _utf16("加粗的一条")),
        (html, "HTML Format", _cf_html("<b>加粗的一条</b>")),
        (rtf, "Rich Text Format", rb"{\rtf1\ansi{\fonttbl{\f0 Arial;}}\f0\b bold\par}" + b"\0"),
    ])
    plain = manager.add_item("纯文本 <x>")
    files = manager.add_item('{"files": ["C:\\\\Windows\\\\win.ini"]}', "file")

    assert manager.paste_items([rich, plain, files], separator="\n") == [rich, plain, files]
    assert _read(CF_UNICODETEXT) == "加粗的一条\r\n纯文本 <x>"
    assert _fragment(_read(html)) == "<b>加粗的一条</b><br>纯文本 &lt;x&gt;"
    assert b"bold" in _read(rtf) and "\\u32431?".encode() in _read(rtf)  # 「纯」
    assert _read(15) == ("C:\\Windows\\win.ini",)
    time.sleep(0.6)
    assert records == []


def test_merged_paste_goes_into_history_only_when_asked(monitor):
    manager, records = monitor
    first, second = manager.add_item("甲"), manager.add_item("乙")
    assert manager.paste_items([first, second], separator="、", keep_in_history=True)
    assert _wait_until(lambda: len(records) == 1)
    assert records[0].content == "甲、乙"


def test_selected_images_are_stitched_into_one(monitor):
    import io
    from PIL import Image
    manager, records = monitor
    png = _register("PNG")
    ids = []
    for size, color in (((40, 10), (255, 0, 0, 255)), ((20, 30), (0, 0, 255, 255))):
        item_id = manager.add_item(f"[{size[0]}x{size[1]}]", "image")
        manager.insert_formats(item_id, [(png, "PNG", _png(*size, color))])
        ids.append(item_id)

    assert manager.paste_items(ids) == ids
    image = Image.open(io.BytesIO(_read(png)))
    assert image.size == (40, 40)
    assert image.getpixel((0, 0)) == (255, 0, 0, 255)
    assert image.getpixel((0, 39)) == (0, 0, 255, 255)
    assert image.getpixel((39, 39))[3] == 0  # 留白透明

    assert manager.paste_items(ids, layout="horizontal") == ids
    assert Image.open(io.BytesIO(_read(png))).size == (60, 30)
    # 只认位图的程序拿到的是同一张图
    dib = _read(8)
    assert struct.unpack("<ii", dib[4:12]) == (60, 30)
    time.sleep(0.6)
    assert records == []


def test_merged_items_move_to_the_top_in_their_own_order(monitor):
    manager, _ = monitor
    ids = [manager.add_item(text) for text in ("一", "二", "三", "四")]
    assert manager.paste_items([ids[2], ids[0]], move_to_top=True)
    assert [item.content for item in manager.get_history(0, 10).items] == ["三", "一", "四", "二"]


def test_history_queries_do_not_deadlock_with_the_monitor(tmp_path):
    """监听线程不能拿着数据库锁去等 GIL：主线程正持 GIL 查历史时，两边会互相等死。"""
    script = tmp_path / "deadlock_check.py"
    script.write_text(
        "import faulthandler, subprocess, sys, time\n"
        "import pyclipboard\n"
        "writer_code = ('import time, win32clipboard as cb\\n'\n"
        "               'end = time.time() + 4\\n'\n"
        "               'i = 0\\n'\n"
        "               'while time.time() < end:\\n'\n"
        "               '    try:\\n'\n"
        "               '        cb.OpenClipboard(); cb.EmptyClipboard()\\n'\n"
        "               '        cb.SetClipboardData(13, (\"第%d条\" % i).encode(\"utf-16-le\") + bytes(2)); cb.CloseClipboard()\\n'\n"
        "               '    except Exception:\\n'\n"
        "               '        pass\\n'\n"
        "               '    i += 1\\n'\n"
        "               '    time.sleep(0.15)\\n')\n"
        f"manager = pyclipboard.ClipboardManager(db_path={str(tmp_path / 'c.db')!r})\n"
        "calls = []\n"
        "manager.start_monitor(callback=lambda item: calls.append(item.id))\n"
        "writer = subprocess.Popen([sys.executable, '-c', writer_code])\n"
        "end = time.time() + 4.5\n"
        "while time.time() < end:\n"
        "    faulthandler.dump_traceback_later(5, exit=True)\n"
        "    manager.get_history(0, 50)\n"
        "faulthandler.cancel_dump_traceback_later()\n"
        "writer.wait()\n"
        "manager.stop_monitor()\n"
        "print('callbacks', len(calls))\n",
        encoding="utf-8",
    )
    done = subprocess.run([sys.executable, str(script)], capture_output=True, text=True,
                          encoding="utf-8", timeout=60)
    assert done.returncode == 0, done.stderr
    assert int(done.stdout.split()[-1]) >= 10
