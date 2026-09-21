"""进程内记忆的上次截图区域（虚拟桌面绝对坐标）。

只保留最近一份，不落盘——重启应用后即清空，纯粹是同一次运行期间、
截图会话之间的临时记忆，供"恢复上次选区"快捷键使用。
"""

from typing import Optional

from PySide6.QtCore import QRect

_last_region: Optional[QRect] = None


def set_last_region(rect: QRect) -> None:
    global _last_region
    _last_region = QRect(rect)


def get_last_region() -> Optional[QRect]:
    return QRect(_last_region) if _last_region is not None else None
