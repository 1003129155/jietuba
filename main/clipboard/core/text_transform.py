# -*- coding: utf-8 -*-
"""纯文本内容加工模块。

提供一系列无副作用的文本转换函数，供"特殊粘贴"菜单调用。
所有函数均为纯函数：输入原始文本，返回转换后的文本。
"""

import re
from collections.abc import Callable
from datetime import datetime


def to_uppercase(text: str) -> str:
    """全部大写。"""
    return text.upper()


def to_lowercase(text: str) -> str:
    """全部小写。"""
    return text.lower()


def capitalize_words(text: str) -> str:
    """每个单词首字母大写（Title Case）。"""
    # 使用 str.title() 处理，但会正确处理已有大写的情况
    return text.title()


def capitalize_sentences(text: str) -> str:
    """句首字母大写。

    以 . ! ? 后跟空白作为句子分隔符，将每句首字母转为大写。
    """
    # 按句子分隔符切分（保留分隔符）
    pattern = r'([.!?]+\s*)'
    parts = re.split(pattern, text)

    result: list[str] = []
    i = 0
    while i < len(parts):
        segment = parts[i]
        # 检查是否匹配分隔符模式
        if re.fullmatch(pattern, segment):
            result.append(segment)
        else:
            # 句子内容：首字母大写
            if segment:
                result.append(segment[0].upper() + segment[1:])
            else:
                result.append(segment)
        i += 1

    return "".join(result)


def toggle_case(text: str) -> str:
    """反转大小写：大写 → 小写，小写 → 大写。"""
    return text.swapcase()


def _parse_sql_values(text: str) -> list[str]:
    """按空白、逗号或分号拆分 SQL IN 值。"""
    return [part for part in re.split(r"[\s,;]+", text) if part]


def _quote_sql_value(value: str) -> str:
    """为 SQL 字符串值添加单引号，并转义值中的单引号。"""
    return "'" + value.replace("'", "''") + "'"


def _format_sql_values(values: list[str], max_line_length: int) -> str:
    """将值格式化为带引号的 SQL 列表，只在值与值之间换行。"""
    if max_line_length <= 0:
        raise ValueError("max_line_length 必须大于 0")
    if not values:
        return ""

    quoted_values = [_quote_sql_value(value) for value in values]
    lines: list[str] = []
    current_line = ""

    for index, quoted_value in enumerate(quoted_values):
        is_last = index == len(quoted_values) - 1
        token = quoted_value if is_last else f"{quoted_value},"
        separator = " " if current_line else ""
        candidate = f"{current_line}{separator}{token}"

        if len(candidate) <= max_line_length or not current_line:
            current_line = candidate
            continue

        lines.append(current_line)
        current_line = token

    lines.append(current_line)
    return "\n".join(lines)


def to_sql_in_clause(text: str, max_line_length: int = 80) -> str:
    """将文本转换为可放入 SQL IN 括号内的值列表。

    按空白、逗号或分号切分输入，为每个值添加单引号并转义值中的
    单引号。输出优先控制在指定行宽内，只在值与值之间换行；单个值
    过长时允许该行超过限制。结果不包含 IN 关键字和外层括号。

    示例：
        "hello world  foo   bar" → 'hello', 'world', 'foo', 'bar'
        "a,b,c" → 'a', 'b', 'c'
        "1\n2\n3" → '1', '2', '3'

    Args:
        text: 待转换的原始文本。
        max_line_length: 期望的最大行宽，必须大于 0，默认 80。

    Returns:
        格式化后的 SQL 值列表。
    """
    return _format_sql_values(_parse_sql_values(text), max_line_length)


def remove_line_breaks(text: str) -> str:
    """移除所有换行符（\r\n、\n、\r），用空格替代。"""
    return re.sub(r'[\r\n]+', ' ', text)


def append_current_time(text: str) -> str:
    """在文本末尾追加当前日期时间。

    格式：原文本 + " " + YYYY-MM-DD HH:MM:SS
    """
    now = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    if text and not text.endswith(('\n', ' ', '\t')):
        return f"{text} {now}"
    return f"{text}{now}"


# 转换函数注册表：key → 转换函数。显示名称由菜单层维护。
TRANSFORM_REGISTRY: dict[str, Callable[[str], str]] = {
    "transform_uppercase": to_uppercase,
    "transform_lowercase": to_lowercase,
    "transform_capitalize_words": capitalize_words,
    "transform_capitalize_sentences": capitalize_sentences,
    "transform_toggle_case": toggle_case,
    "transform_sql_in": to_sql_in_clause,
    "transform_remove_linebreaks": remove_line_breaks,
    "transform_append_time": append_current_time,
}


__all__ = [
    "TRANSFORM_REGISTRY",
    "append_current_time",
    "capitalize_sentences",
    "capitalize_words",
    "remove_line_breaks",
    "to_lowercase",
    "to_sql_in_clause",
    "to_uppercase",
    "toggle_case",
]
