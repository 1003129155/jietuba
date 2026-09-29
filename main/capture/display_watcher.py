"""
显示器配置变化监听 - 插拔显示器、改分辨率、开关 HDR
"""

from ctypes import wintypes

from PySide6.QtCore import QAbstractNativeEventFilter, QCoreApplication, QObject, QTimer
from PySide6.QtGui import QWindow

WM_DISPLAYCHANGE = 0x007E


class _DisplayChangeFilter(QAbstractNativeEventFilter):
    def __init__(self, on_message):
        super().__init__()
        self._on_message = on_message

    def nativeEventFilter(self, event_type, message):
        if event_type in (b"windows_generic_MSG", b"windows_dispatcher_MSG"):
            if wintypes.MSG.from_address(int(message)).message == WM_DISPLAYCHANGE:
                self._on_message()
        return False, 0


class DisplayChangeWatcher(QObject):
    """显示器配置变化后在 GUI 线程上调用 on_change。

    WM_DISPLAYCHANGE 只广播给顶层窗口，托盘程序可能一个可见窗口都没有，所以自建一个不显示的
    原生窗口保证收得到。一次配置变化常连发几条，等 settle_ms 内不再有新消息才回调一次。
    """

    def __init__(self, on_change, parent=None, settle_ms=500):
        super().__init__(parent)
        self._timer = QTimer(self)
        self._timer.setSingleShot(True)
        self._timer.setInterval(settle_ms)
        self._timer.timeout.connect(on_change)
        self._window = QWindow()
        self._window.create()
        self._filter = _DisplayChangeFilter(self._timer.start)
        QCoreApplication.instance().installNativeEventFilter(self._filter)

    def close(self):
        QCoreApplication.instance().removeNativeEventFilter(self._filter)
        self._timer.stop()
        self._window.destroy()
