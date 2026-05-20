"""clipboard controllers 兼容导出。"""

from .clipboard_controller import (
    ClipboardController,
    ContextMenuData,
    MenuAction,
    calc_sidebar_capacity,
    calc_topbar_capacity,
    get_foreground_window,
    send_ctrl_v,
    set_foreground_window,
)
from .selection_manager import SelectionManager

__all__ = [
    "ClipboardController",
    "ContextMenuData",
    "MenuAction",
    "SelectionManager",
    "calc_sidebar_capacity",
    "calc_topbar_capacity",
    "get_foreground_window",
    "send_ctrl_v",
    "set_foreground_window",
]