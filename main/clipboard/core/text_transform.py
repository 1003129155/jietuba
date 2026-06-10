# -*- coding: utf-8 -*-
"""纯文本内容加工模块。

提供一系列无副作用的文本转换函数，供"特殊粘贴"菜单调用。
所有函数均为纯函数：输入原始文本，返回转换后的文本。
"""

import re
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


def to_sql_in_clause(text: str) -> str:
    """将文本转换为 SQL IN 子句格式。

    每遇到不连续的区域就切分一次，然后给每个切好的单元前后加单引号，
    用逗号连接，最终包装为 IN (...) 格式。

    示例：
        "hello world  foo   bar" → IN ('hello', 'world', 'foo', 'bar')
        "a,b,c" → IN ('a', 'b', 'c')
        "1\n2\n3" → IN ('1', '2', '3')
    """
    # 按任意空白、逗号、换行等分隔符切分
    parts = re.split(r'[\s,;\n\r\t]+', text)
    # 过滤空字符串
    parts = [p.strip() for p in parts if p.strip()]
    if not parts:
        return "IN ()"

    quoted = ", ".join(f"'{p}'" for p in parts)
    return f"{quoted}"


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


# 转换函数注册表：key → (显示名, 转换函数)
TRANSFORM_REGISTRY: dict[str, tuple[str, callable]] = {
    "transform_uppercase":       ("All Uppercase",       to_uppercase),
    "transform_lowercase":       ("All Lowercase",       to_lowercase),
    "transform_capitalize_words": ("Capitalize Words",   capitalize_words),
    "transform_capitalize_sentences": ("Capitalize Sentences", capitalize_sentences),
    "transform_toggle_case":     ("Toggle Case",         toggle_case),
    "transform_sql_in":          ("SQL IN Clause",       to_sql_in_clause),
    "transform_remove_linebreaks": ("Remove Line Breaks", remove_line_breaks),
    "transform_append_time":     ("Paste with Current Time", append_current_time),
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
