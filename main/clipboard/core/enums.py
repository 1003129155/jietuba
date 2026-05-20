"""clipboard core 枚举定义。"""

from enum import IntEnum


class GroupType(IntEnum):
    """分组类型。"""

    NORMAL = 0
    FILE = 1


__all__ = ["GroupType"]