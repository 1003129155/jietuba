"""
Qt 工具函数
"""

import warnings

import shiboken6
from PySide6.QtCore import Qt
from PySide6.QtGui import QWindow
from PySide6.QtWidgets import QApplication


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


def _window_chain(window) -> list:
    """窗口自己加上它的父窗口、所属窗口，一直到顶。"""
    chain = []
    while isinstance(window, QWindow):
        chain.append(window)
        window = window.parent(QWindow.AncestorMode.IncludeTransients)
    return chain


def blocking_modal(widget=None):
    """挡住 widget 所在窗口输入的可见模态窗口，没有则返回 None。

    规则与 Qt 一致：应用模态挡住其余所有窗口；窗口模态只挡它所属的那条窗口链，以及挂在
    这条链上的其他窗口。widget 为 None 时按新建的无父顶层窗口判断，只有应用模态挡得住它。
    看可见的顶层窗口而不是 activeModalWidget()，因为模态窗口的 Show 事件早于它在 Qt 里登记。
    """
    target = _window_chain(widget.window().windowHandle()) if widget is not None else []
    for candidate in QApplication.topLevelWidgets():
        if not (candidate.isVisible() and candidate.isModal()):
            continue
        handle = candidate.windowHandle()
        if handle is None or handle in target:
            continue
        if candidate.windowModality() == Qt.WindowModality.ApplicationModal:
            return candidate
        if any(window in target for window in _window_chain(handle)):
            return candidate
    return None
 