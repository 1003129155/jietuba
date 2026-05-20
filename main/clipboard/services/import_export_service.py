# -*- coding: utf-8 -*-

"""文本导入导出服务。

负责将分组中的文本条目导出为 CSV，以及从 CSV 读取内容并按分组
回写到 manager。这里不处理 UI，只封装数据整理与导入流程。
"""

import csv
import io
from typing import Iterable


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
    with open(file_path, "w", newline="", encoding=encoding, errors="replace") as f:
        writer = csv.writer(f)
        writer.writerow(list(header))
        writer.writerows(rows)


def read_import_rows(file_path: str) -> list[tuple[str, str, str]]:
    """从 CSV 文件读取可导入数据。"""
    content = None
    for encoding in DEFAULT_IMPORT_ENCODINGS:
        try:
            with open(file_path, "r", newline="", encoding=encoding) as f:
                content = f.read()
            break
        except (UnicodeDecodeError, LookupError):
            continue

    if content is None:
        with open(file_path, "r", newline="", encoding="utf-8", errors="replace") as f:
            content = f.read()

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
