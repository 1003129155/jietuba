# -*- coding: utf-8 -*-

"""文本导入导出服务。

负责将分组中的文本条目导出为 CSV，以及从 CSV 读取内容并按分组
回写到 manager。这里不处理 UI，只封装数据整理与导入流程。
"""

import csv
import ctypes
import io
import sys
from typing import Iterable, Optional


DEFAULT_IMPORT_ENCODINGS = (
    "utf-8-sig",
    "utf-8",
    "shift_jis",
    "cp932",
    "gbk",
    "gb2312",
    "gb18030",
    "cp1252",
    "latin-1",
)


# shift_jis、gbk 等按 Excel 读写的 Windows 代码页转换。Python 自带的严格 JIS/GBK 表会把 ～ ① 髙 €
# 写成 ?、把 ～ − 读成 〜 −，也解不了 Excel 写的单字节 €（0x80）
_CODE_PAGES = {"shift_jis": 932, "cp932": 932, "gbk": 936, "gb2312": 936, "gb18030": 54936, "cp1252": 1252}
_WC_NO_BEST_FIT_CHARS = 0x400
_MB_ERR_INVALID_CHARS = 0x8
# Python 的 cp1252 没有定义这几个字节，Windows 会直接映射过去
_CP1252_UNDEFINED = b"\x81\x8d\x8f\x90\x9d"


def _windows_encode(text: str, page: int) -> bytes:
    kernel32 = ctypes.windll.kernel32
    if page != 54936:
        # 这些代码页没有 BMP 以外的字符，每个字符换成一个 ?，而不是每个 UTF-16 单元一个
        text = "".join("?" if ord(char) > 0xFFFF else char for char in text)
    flags, default = (0, None) if page == 54936 else (_WC_NO_BEST_FIT_CHARS, b"?")
    size = len(text.encode("utf-16-le", errors="surrogatepass")) // 2
    needed = kernel32.WideCharToMultiByte(page, flags, text, size, None, 0, default, None)
    buffer = ctypes.create_string_buffer(needed)
    kernel32.WideCharToMultiByte(page, flags, text, size, buffer, needed, default, None)
    return buffer.raw[:needed]


def _windows_decode(data: bytes, page: int) -> Optional[str]:
    kernel32 = ctypes.windll.kernel32
    needed = kernel32.MultiByteToWideChar(page, _MB_ERR_INVALID_CHARS, data, len(data), None, 0)
    if needed <= 0:
        return None
    buffer = ctypes.create_unicode_buffer(needed)
    kernel32.MultiByteToWideChar(page, _MB_ERR_INVALID_CHARS, data, len(data), buffer, needed)
    return buffer[:needed]


def _encode(text: str, encoding: str) -> bytes:
    page = _CODE_PAGES.get(encoding)
    if page is None or sys.platform != "win32":
        return text.encode(encoding, errors="replace")
    return _windows_encode(text, page) if text else b""


def _decode(data: bytes, encoding: str) -> Optional[str]:
    """严格解码，字节不属于该编码时返回 None。"""
    page = _CODE_PAGES.get(encoding)
    if page is None or sys.platform != "win32":
        try:
            return data.decode(encoding)
        except (UnicodeDecodeError, LookupError):
            return None
    if page == 1252 and any(byte in _CP1252_UNDEFINED for byte in data):
        return None
    if not data:
        return ""
    if page == 936:
        # GB18030 把 cp936 读成私用区的字读成正字（A2E3 的 € 等），单字节 €（0x80）只有 cp936 认
        text = _windows_decode(data, 54936)
        return text if text is not None else _windows_decode(data, 936)
    return _windows_decode(data, page)


def collect_text_export_rows(manager, page_size: int = 500) -> list[list[str]]:
    """从 manager 收集可导出的纯文本条目。"""
    rows: list[list[str]] = []
    groups = manager.get_groups()

    for group in groups:
        offset = 0
        while True:
            items = manager.get_by_group(group.id, offset=offset, limit=page_size)
            if not items:
                break
            for item in items:
                if item.content_type == "text" and item.content:
                    rows.append([group.name, item.content, item.title or ""])
            if len(items) < page_size:
                break
            offset += page_size

    return rows


def write_csv_rows(file_path: str, header: Iterable[str], rows: Iterable[Iterable[str]], encoding: str) -> None:
    """将表头和数据行写入 CSV 文件。"""
    buffer = io.StringIO(newline="")
    writer = csv.writer(buffer)
    writer.writerow(list(header))
    writer.writerows(rows)
    with open(file_path, "wb") as f:
        f.write(_encode(buffer.getvalue(), encoding))


def read_import_rows(file_path: str) -> list[tuple[str, str, str]]:
    """从 CSV 文件读取可导入数据。"""
    with open(file_path, "rb") as f:
        data = f.read()
    content = None
    for encoding in DEFAULT_IMPORT_ENCODINGS:
        content = _decode(data, encoding)
        if content is not None:
            break

    if content is None:
        content = data.decode("utf-8", errors="replace")

    rows: list[tuple[str, str, str]] = []
    reader = csv.reader(io.StringIO(content))
    next(reader, None)
    for row in reader:
        if len(row) < 2:
            continue
        group_name = row[0].strip()
        item_content = row[1] if len(row) > 1 else ""
        title = row[2].strip() if len(row) > 2 else ""
        if group_name and item_content:
            rows.append((group_name, item_content, title))
    return rows


def import_text_rows(manager, rows: list[tuple[str, str, str]]) -> int:
    """将 CSV 文本行导入 manager，复用已有同名分组。"""
    existing_groups = manager.get_groups()
    existing_group_name_to_id = {g.name: g.id for g in existing_groups}
    import_group_name_to_target: dict[str, int] = {}

    imported_count = 0
    for group_name, content, title in reversed(rows):
        if group_name not in import_group_name_to_target:
            existing_group_id = existing_group_name_to_id.get(group_name)
            if existing_group_id is not None:
                import_group_name_to_target[group_name] = existing_group_id
            else:
                new_group_id = manager.create_group(group_name)
                if not new_group_id:
                    continue
                existing_group_name_to_id[group_name] = new_group_id
                import_group_name_to_target[group_name] = new_group_id

        group_id = import_group_name_to_target[group_name]
        title_param = title if title else None
        item_id = manager.add_item(content, "text", title=title_param)
        if item_id:
            manager.move_to_group(item_id, group_id)
            imported_count += 1

    return imported_count
