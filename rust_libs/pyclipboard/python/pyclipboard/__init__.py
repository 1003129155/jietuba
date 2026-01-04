"""
pyclipboard - Python 剪贴板管理库（Rust 实现）

使用示例:

    基础操作:
    >>> import pyclipboard
    >>> pyclipboard.set_clipboard_text("Hello")
    >>> print(pyclipboard.get_clipboard_text())
    Hello

    历史管理:
    >>> from pyclipboard import PyClipboardManager
    >>> manager = PyClipboardManager()
    >>> manager.add_item("测试内容")
    >>> for item in manager.get_history():
    ...     print(item.content)
"""

# 如果需要 Python 端的额外封装，可以在这里添加
