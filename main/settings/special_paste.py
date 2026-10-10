# -*- coding: utf-8 -*-
"""特殊粘贴菜单的预置选项、顺序和启用状态。"""

import json
from dataclasses import dataclass

SETTING_KEY = "clipboard_special_paste"


@dataclass(frozen=True)
class PasteOption:
    key: str
    label: str
    description: str
    content_type: str = "text"
    group: str = "text"
    enabled: bool = True


BUILTIN_OPTIONS = (
    PasteOption("special_paste_plain_text", "Paste Plain Text", "Paste without text formatting.", group="plain"),
    PasteOption("transform_uppercase", "All Uppercase", "Convert letters to uppercase.", group="case"),
    PasteOption("transform_lowercase", "All Lowercase", "Convert letters to lowercase.", group="case"),
    PasteOption("transform_capitalize_words", "Capitalize Words", "Capitalize the first letter of each word.", group="case"),
    PasteOption("transform_capitalize_sentences", "Capitalize Sentences", "Capitalize the first letter of each sentence.", group="case"),
    PasteOption("transform_toggle_case", "Toggle Case", "Swap uppercase and lowercase letters.", group="case"),
    PasteOption("transform_sql_in", "SQL IN Clause", "Format values as a quoted SQL list.", group="sql"),
    PasteOption("transform_remove_linebreaks", "Remove Line Breaks", "Replace line breaks with spaces."),
    PasteOption("transform_remove_whitespace", "Remove All Whitespace", "Remove spaces, tabs and line breaks."),
    PasteOption("transform_extract_digits", "Extract Digits", "Keep only digits 0–9.", enabled=False),
    PasteOption("transform_extract_letters", "Extract Letters", "Keep only English letters A–Z and a–z.", enabled=False),
    PasteOption("transform_append_time", "Paste with Current Time", "Append the current date and time."),
    PasteOption("special_paste_in_order", "Paste in Order", "Paste without moving the item to the top.", group="order"),
    PasteOption("file_paste_names", "File Name", "Paste file names without their folder paths.", content_type="file"),
    PasteOption("file_paste_links", "File Link", "Paste full file paths.", content_type="file"),
)
OPTIONS_BY_KEY = {option.key: option for option in BUILTIN_OPTIONS}


def default_entries():
    return [(option.key, option.enabled) for option in BUILTIN_OPTIONS]


def normalize(entries):
    """保留已配置的顺序和开关，忽略无效项，补齐新增预置；允许全部关闭。"""
    if not isinstance(entries, (list, tuple)):
        entries = ()
    result = []
    seen = set()
    for entry in entries:
        if not isinstance(entry, (list, tuple)) or len(entry) != 2:
            continue
        key, enabled = entry
        if not isinstance(key, str) or key not in OPTIONS_BY_KEY or key in seen:
            continue
        seen.add(key)
        if not isinstance(enabled, bool):
            enabled = OPTIONS_BY_KEY[key].enabled
        result.append((key, enabled))
    result.extend((option.key, option.enabled) for option in BUILTIN_OPTIONS if option.key not in seen)
    return result


def load(config_manager):
    raw = config_manager.get_app_setting(SETTING_KEY, "") if config_manager is not None else ""
    try:
        stored = json.loads(raw) if raw else None
    except (TypeError, ValueError):
        stored = None
    return normalize(stored)


def save(config_manager, entries):
    config_manager.set_app_setting(SETTING_KEY, json.dumps(normalize(entries)))
