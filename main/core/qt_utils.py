"""
Qt 信号工具函数
"""


def safe_disconnect(signal, slot=None):
    """安全断开信号连接，忽略已断开或无效的连接。

    Args:
        signal: Qt Signal 对象
        slot: 可选，指定要断开的 slot。为 None 时断开该信号的所有连接。
    """
    try:
        if slot is not None:
            signal.disconnect(slot)
        else:
            signal.disconnect()
    except (RuntimeError, TypeError):
        pass
 