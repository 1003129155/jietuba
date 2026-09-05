"""main/capture/ 目录下 log_* 调用的中→英翻译表。"""

TRANSLATIONS: dict[str, str] = {
    "win32gui 未安装，智能选区功能不可用": "win32gui is not installed, smart selection is unavailable",
    "使用偏移: ({offset_x}, {offset_y})": "Using offset: ({offset_x}, {offset_y})",
    "获取窗口类名 hwnd={hwnd}": "Failed to get window class name for hwnd={hwnd}",
    "处理窗口时出错: {e}": "Error processing window: {e}",
    "找到 {count} 个有效窗口": "Found {count} valid windows",
    "检测到的窗口列表（前5个）:": "Detected window list (first 5):",
    "{index}. 标题: {title}, 大小: {width}x{height}, 位置: ({x}, {y})": "{index}. Title: {title}, Size: {width}x{height}, Position: ({x}, {y})",
    "枚举窗口失败: {e}": "Failed to enumerate windows: {e}",
    "鼠标({x}, {y})处找到窗口: '{title}', 大小: {width}x{height}, Z-order: {idx}": "Found window at cursor ({x}, {y}): '{title}', size: {width}x{height}, Z-order: {idx}",
    "在鼠标位置({x}, {y})未找到有效窗口，返回备选矩形": "No valid window found at cursor position ({x}, {y}), returning fallback rect",
    "获取虚拟屏幕尺寸": "Failed to get virtual screen size",
    "降级获取主显示器尺寸": "Falling back to primary monitor size",
    "查找窗口失败: {e}": "Failed to find window: {e}",
}
