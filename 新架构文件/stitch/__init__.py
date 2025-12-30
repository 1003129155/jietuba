"""
长截图拼接模块
"""

# 延迟导入 ScrollCaptureWindow，只在实际使用时导入
# 这样可以避免在导入 stitch 模块时就需要 PyQt6
def __getattr__(name):
    if name == 'ScrollCaptureWindow':
        from .scroll_window import ScrollCaptureWindow
        return ScrollCaptureWindow
    raise AttributeError(f"module '{__name__}' has no attribute '{name}'")
