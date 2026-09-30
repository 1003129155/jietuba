"""
Qt 工具函数
"""

import warnings

import shiboken6


def safe_disconnect(signal, slot=None):
    """安全断开信号连接，忽略已断开或无效的连接。

    Args:
        signal: Qt Signal 对象
        slot: 可选，指定要断开的 slot。为 None 时断开该信号的所有连接。
    """
    try:
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", RuntimeWarning)
            if slot is not None:
                signal.disconnect(slot)
            else:
                signal.disconnect()
    except (RuntimeError, TypeError):
        pass


def layout_widgets(layout) -> list:
    """布局里的控件，按布局顺序。

    PySide 会把 itemAt() 返回的条目包装挂在布局上，Qt 删掉条目后它仍算有效，同一地址上的
    新对象会被认成它。所以取完控件就把条目包装作废。
    """
    widgets = []
    for index in range(layout.count()):
        item = layout.itemAt(index)
        widget = item.widget()
        shiboken6.invalidate(item)
        if widget is not None:
            widgets.append(widget)
    return widgets
 