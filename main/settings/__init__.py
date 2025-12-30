"""
设置模块

统一的配置管理系统，管理：
1. 工具设置（画笔、矩形等绘图工具的颜色、大小等）
2. 应用设置（热键、保存路径、OCR等应用级配置）

使用方式：
    from settings import get_tool_settings_manager
    
    manager = get_tool_settings_manager()
    
    # 工具设置
    color = manager.get_color("pen")
    manager.set_stroke_width("rect", 10)
    
    # 应用设置
    hotkey = manager.get_hotkey()
    manager.set_screenshot_save_path("/path/to/save")
"""

from .tool_settings import ToolSettingsManager, get_tool_settings_manager

__all__ = ['ToolSettingsManager', 'get_tool_settings_manager']
