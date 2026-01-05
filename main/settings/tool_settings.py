"""
统一设置管理器 - 集中管理所有默认配置

本文件是整个应用的配置中心，包含：

1. 工具设置（绘图工具的默认参数）
   - DEFAULT_SETTINGS 字典定义了所有绘图工具的默认值
   - 包括：画笔、荧光笔、矩形、椭圆、箭头、文字、序号等
   - 每个工具都有：颜色、宽度、透明度等参数

2. 应用级别设置（设置界面中的开关和配置）
   - APP_DEFAULT_SETTINGS 字典定义了所有应用级别的默认值
   - 包括：智能选区、任务栏按钮、日志、截图保存、长截图、OCR、钉图等
   - 这些是设置界面中各种开关和输入框的默认值

使用方式：
    # 修改默认值，直接编辑对应字典即可
    DEFAULT_SETTINGS["pen"]["color"] = "#0000FF"  # 画笔默认蓝色
    APP_DEFAULT_SETTINGS["ocr_enabled"] = False   # OCR默认关闭
    
    # 重置为默认值
    config_manager.reset_all_settings()           # 重置所有设置
    config_manager.reset_app_settings()           # 只重置应用设置
    config_manager.reset_all()                    # 只重置工具设置

每个工具都有独立的设置，并且会记忆最后使用的状态
"""

import os
from typing import Dict, Any, Optional
from PyQt6.QtCore import QSettings, pyqtSignal, QObject
from PyQt6.QtGui import QColor


class ToolSettings:
    """单个工具的设置数据类"""
    
    def __init__(self, tool_id: str, defaults: Dict[str, Any]):
        """
        Args:
            tool_id: 工具ID（pen, rect, ellipse等）
            defaults: 默认设置字典
        """
        self.tool_id = tool_id
        self.defaults = defaults
        self._current = defaults.copy()
    
    def get(self, key: str, default=None) -> Any:
        """获取设置值"""
        return self._current.get(key, default if default is not None else self.defaults.get(key))
    
    def set(self, key: str, value: Any):
        """设置值"""
        self._current[key] = value
    
    def update(self, **kwargs):
        """批量更新设置"""
        self._current.update(kwargs)
    
    def reset_to_defaults(self):
        """重置为默认值"""
        self._current = self.defaults.copy()
    
    def to_dict(self) -> Dict[str, Any]:
        """导出为字典"""
        return self._current.copy()
    
    def from_dict(self, data: Dict[str, Any]):
        """从字典导入"""
        self._current.update(data)


class ToolSettingsManager(QObject):
    """
    工具设置管理器
    
    功能：
    1. 为每个工具维护独立的设置（颜色、尺寸、透明度等）
    2. 自动保存和加载每个工具的最后使用状态
    3. 提供默认设置和重置功能
    """
    
    # 信号：当工具设置改变时发出
    settings_changed = pyqtSignal(str, dict)  # (tool_id, settings_dict)
    
    # 各工具的默认设置
    DEFAULT_SETTINGS = {
        "pen": {
            "color": "#FF0000",  # 红色
            "stroke_width": 12,
            "opacity": 1.0,
        },
        "highlighter": {
            "color": "#FFFF00",  # 黄色
            "stroke_width": 15,
            "opacity": 1.0,
        },
        "rect": {
            "color": "#FF0000",  # 红色
            "stroke_width": 9,
            "opacity": 1.0,
        },
        "ellipse": {
            "color": "#FF0000",  # 红色
            "stroke_width": 9,
            "opacity": 1.0,
        },
        "arrow": {
            "color": "#FF0000",  # 红色
            "stroke_width": 9,
            "opacity": 1.0,
            "arrow_size": 9,  # 箭头大小
        },
        "text": {
            "color": "#000000",  # 黑色
            "font_size": 14,
            "opacity": 1.0,
            "font_family": "微软雅黑",
        },
        "number": {
            "color": "#FF0000",  # 红色
            "font_size": 10,
            "opacity": 1.0,
            "circle_radius": 20,
            "stroke_width": 12,
        },
        "eraser": {
            "stroke_width": 25,  # 橡皮擦大小（宽度）
            "opacity": 1.0,      # 占位参数（橡皮擦不需要透明度）
        },
    }
    
    # 应用级别的默认设置（按照设置界面的页面顺序排列）
    APP_DEFAULT_SETTINGS = {
        # ==================== 1. ⌨️ 快捷键设置 ====================win
        "hotkey": "ctrl+1",                        # 全局截图热键
        "clipboard_hotkey": "ctrl+2",    # 打开剪贴板管理器的快捷键
        # ==================== 2. 📸 长截图设置 ====================
        "long_stitch_engine": "hash_rust",     # 长截图引擎（hash_rust / hash_python / simple）
        "long_stitch_debug": False,            # 长截图调试模式
        "scroll_cooldown": 0.15,               # 滚动后等待时间（秒，0.05-1.0）
        
        # ==================== 3. 🎯 智能选择设置 ====================
        "smart_selection": False,              # 智能选区（窗口/控件识别）
        
        # ==================== 4. [SAVE] 截图保存设置 ====================
        "screenshot_save_enabled": True,       # 自动保存截图
        "screenshot_save_path": os.path.join(os.path.expanduser("~"), "Desktop", "スクショ"),  # 默认保存路径
        
        # ==================== 5. 🎯 OCR设置 ====================
        "ocr_enabled": True,                   # OCR功能启用
        "ocr_engine": "windos_ocr",            # OCR引擎类型 (windos_ocr 推荐, windows_media_ocr 备用)
        "ocr_grayscale": False,                # OCR灰度转换（Windows OCR 不需要）
        "ocr_upscale": True,                   # OCR图像放大（提升小字识别率）
        "ocr_upscale_factor": 2.0,             # OCR放大倍数（1.0-3.0）
        
        # ==================== 6. 📝 日志设置 ====================
        "log_enabled": True,                   # 日志启用
        "log_dir": os.path.join(os.path.expanduser("~"), "AppData", "Local", "Jietuba", "Logs"),
        "log_level": "INFO",                # 日志等级: DEBUG, INFO, WARNING, ERROR
        "log_retention_days": 7,               # 日志保留天数（0表示永久保留）
        
        # ==================== 7. ⚙️ 其他设置 ====================
        "show_main_window": False,             # 运行后自动弹出窗口显示（默认后台启动）
        "language": "ja",                      # 界面语言（ja/en/zh）
        
        # ==================== 钉图设置（在"其他"页面或独立页面） ====================
        "pin_auto_toolbar": False,              # 钉图自动显示工具栏
        "pin_default_opacity": 1.0,            # 钉图默认透明度（0.1-1.0）
        
        # ==================== 8. 🌐 翻译设置 ====================
        "deepl_api_key": "dfdb66fc-025c-43b5-8196-7daba2c2da7d:fx",  # DeepL API 密钥
        "deepl_use_pro": False,                # 是否使用 Pro 版 API
        "translation_target_lang": "",         # 翻译目标语言（空为跟随系统语言）
        "translation_split_sentences": True,   # 自动分句
        "translation_preserve_formatting": True,  # 保留格式
        
        # ==================== 9. 📋 剪贴板设置 ====================
        "clipboard_enabled": True,             # 剪贴板监听启用
        "clipboard_auto_paste": True,          # 选择后自动粘贴（发送 Ctrl+V）
        "clipboard_history_limit": 500,        # 历史记录数量限制（0 为不限制）
        "clipboard_auto_cleanup": True,        # 自动清理超出限制的记录
        "clipboard_window_width": 450,         # 剪贴板窗口默认宽度
        "clipboard_window_height": 600,        # 剪贴板窗口默认高度
        "clipboard_window_opacity": 0,         # 剪贴板窗口透明度（0=不透明，5/10/15/20/25）
        "clipboard_window_opacity_options": [0, 5, 10, 15, 20, 25],  # 透明度可选项（可在此调整选项）
        "clipboard_paste_with_html": True,     # 粘贴时是否带 HTML 格式
        "clipboard_display_lines": 1,          # 剪贴板项最大显示行数（1/2，内容会自适应）
        "clipboard_line_height_padding": 8,   # 多行显示时的额外行高边距（像素，用于确保完整显示）
    }
    
    def __init__(self):
        super().__init__()
        self.qsettings = QSettings("Jietuba", "ToolSettings")
        self._tool_settings: Dict[str, ToolSettings] = {}
        self._initialize_tools()
    
    @property
    def settings(self):
        """兼容旧代码：返回 QSettings 实例"""
        return self.qsettings
    
    def _initialize_tools(self):
        """初始化所有工具的设置"""
        for tool_id, defaults in self.DEFAULT_SETTINGS.items():
            # 创建工具设置对象
            tool_setting = ToolSettings(tool_id, defaults)
            
            # 从持久化存储加载
            self._load_tool_settings(tool_setting)
            
            self._tool_settings[tool_id] = tool_setting
    
    def _load_tool_settings(self, tool_setting: ToolSettings):
        """从 QSettings 加载工具设置"""
        tool_id = tool_setting.tool_id
        
        # 加载每个设置项
        for key, default_value in tool_setting.defaults.items():
            setting_key = f"tools/{tool_id}/{key}"
            
            # 根据类型加载
            if isinstance(default_value, bool):
                value = self.qsettings.value(setting_key, default_value, type=bool)
            elif isinstance(default_value, int):
                value = self.qsettings.value(setting_key, default_value, type=int)
            elif isinstance(default_value, float):
                value = self.qsettings.value(setting_key, default_value, type=float)
            elif isinstance(default_value, str):
                value = self.qsettings.value(setting_key, default_value, type=str)
            else:
                value = self.qsettings.value(setting_key, default_value)
            
            tool_setting.set(key, value)
    
    def _save_tool_settings(self, tool_setting: ToolSettings):
        """保存工具设置到 QSettings"""
        tool_id = tool_setting.tool_id
        
        for key, value in tool_setting.to_dict().items():
            setting_key = f"tools/{tool_id}/{key}"
            self.qsettings.setValue(setting_key, value)
        
        self.qsettings.sync()
    
    def get_tool_settings(self, tool_id: str) -> Optional[ToolSettings]:
        """获取工具的设置对象"""
        return self._tool_settings.get(tool_id)
    
    def get_setting(self, tool_id: str, key: str, default=None) -> Any:
        """
        获取指定工具的某个设置值
        
        Args:
            tool_id: 工具ID
            key: 设置键名
            default: 默认值
        
        Returns:
            设置值
        """
        tool_setting = self._tool_settings.get(tool_id)
        if tool_setting:
            return tool_setting.get(key, default)
        return default
    
    def set_setting(self, tool_id: str, key: str, value: Any, save_immediately: bool = True):
        """
        设置指定工具的某个设置值
        
        Args:
            tool_id: 工具ID
            key: 设置键名
            value: 设置值
            save_immediately: 是否立即保存到磁盘
        """
        tool_setting = self._tool_settings.get(tool_id)
        if tool_setting:
            tool_setting.set(key, value)
            
            if save_immediately:
                self._save_tool_settings(tool_setting)
            
            # 发出信号
            self.settings_changed.emit(tool_id, tool_setting.to_dict())
    
    def update_settings(self, tool_id: str, save_immediately: bool = True, **kwargs):
        """
        批量更新工具设置
        
        Args:
            tool_id: 工具ID
            save_immediately: 是否立即保存
            **kwargs: 设置键值对
        """
        tool_setting = self._tool_settings.get(tool_id)
        if tool_setting:
            tool_setting.update(**kwargs)
            
            if save_immediately:
                self._save_tool_settings(tool_setting)
            
            # 发出信号
            self.settings_changed.emit(tool_id, tool_setting.to_dict())
    
    def save_all(self):
        """保存所有工具的设置"""
        for tool_setting in self._tool_settings.values():
            self._save_tool_settings(tool_setting)
    
    def reset_tool(self, tool_id: str, save_immediately: bool = True):
        """
        重置工具设置为默认值
        
        Args:
            tool_id: 工具ID
            save_immediately: 是否立即保存
        """
        tool_setting = self._tool_settings.get(tool_id)
        if tool_setting:
            tool_setting.reset_to_defaults()
            
            if save_immediately:
                self._save_tool_settings(tool_setting)
            
            # 发出信号
            self.settings_changed.emit(tool_id, tool_setting.to_dict())
    
    def reset_all(self):
        """重置所有工具设置为默认值"""
        for tool_id in self._tool_settings.keys():
            self.reset_tool(tool_id, save_immediately=False)
        
        self.save_all()
    
    def reset_app_settings(self):
        """重置所有应用级别设置为默认值"""
        for key, default_value in self.APP_DEFAULT_SETTINGS.items():
            # 构建完整的设置键名
            if key.startswith("pin_"):
                setting_key = f"pin/{key[4:]}"  # pin_auto_toolbar -> pin/auto_toolbar
            else:
                setting_key = f"app/{key}"
            
            self.qsettings.setValue(setting_key, default_value)
        
        print("[OK] [设置] 应用设置已重置为默认值")
    
    def get_app_setting(self, key: str, default=None) -> Any:
        """
        获取应用级别设置的通用方法
        
        Args:
            key: 设置键名（不含 app/ 前缀）
            default: 默认值，如果为 None 则使用 APP_DEFAULT_SETTINGS 中的默认值
            
        Returns:
            设置值
        """
        # 确定实际默认值
        if default is None:
            default = self.APP_DEFAULT_SETTINGS.get(key)
        
        # 构建完整的设置键名
        if key.startswith("pin_"):
            setting_key = f"pin/{key[4:]}"
        else:
            setting_key = f"app/{key}"
        
        # 根据默认值的类型确定返回类型
        if default is not None:
            if isinstance(default, bool):
                return self.qsettings.value(setting_key, default, type=bool)
            elif isinstance(default, int):
                return self.qsettings.value(setting_key, default, type=int)
            elif isinstance(default, float):
                return self.qsettings.value(setting_key, default, type=float)
            else:
                return self.qsettings.value(setting_key, default, type=str)
        else:
            return self.qsettings.value(setting_key, default)
    
    def set_app_setting(self, key: str, value: Any):
        """
        设置应用级别设置的通用方法
        
        Args:
            key: 设置键名（不含 app/ 前缀）
            value: 设置值
        """
        # 构建完整的设置键名
        if key.startswith("pin_"):
            setting_key = f"pin/{key[4:]}"
        else:
            setting_key = f"app/{key}"
        
        self.qsettings.setValue(setting_key, value)

    def reset_all_settings(self):
        """重置所有设置（工具设置 + 应用设置）为默认值"""
        self.reset_all()           # 重置工具设置
        self.reset_app_settings()  # 重置应用设置
        print("[OK] [设置] 所有设置已重置为默认值")
    
    def get_color(self, tool_id: str) -> QColor:
        """获取工具的颜色（返回 QColor 对象）"""
        color_str = self.get_setting(tool_id, "color", "#FF0000")
        return QColor(color_str)
    
    def set_color(self, tool_id: str, color: QColor, save_immediately: bool = True):
        """设置工具的颜色"""
        color_str = color.name()  # 转为 #RRGGBB 格式
        self.set_setting(tool_id, "color", color_str, save_immediately)
    
    def get_stroke_width(self, tool_id: str) -> int:
        """获取工具的笔触宽度"""
        return self.get_setting(tool_id, "stroke_width", 3)
    
    def set_stroke_width(self, tool_id: str, width: int, save_immediately: bool = True):
        """设置工具的笔触宽度"""
        self.set_setting(tool_id, "stroke_width", width, save_immediately)
    
    def get_opacity(self, tool_id: str) -> float:
        """获取工具的透明度"""
        return self.get_setting(tool_id, "opacity", 1.0)
    
    def set_opacity(self, tool_id: str, opacity: float, save_immediately: bool = True):
        """设置工具的透明度"""
        self.set_setting(tool_id, "opacity", opacity, save_immediately)
    
    def get_font_size(self, tool_id: str) -> int:
        """获取文字工具的字体大小"""
        return self.get_setting(tool_id, "font_size", 14)
    
    def set_font_size(self, tool_id: str, size: int, save_immediately: bool = True):
        """设置文字工具的字体大小"""
        self.set_setting(tool_id, "font_size", size, save_immediately)
    
    def export_settings(self) -> Dict[str, Dict[str, Any]]:
        """导出所有工具设置（用于备份）"""
        return {
            tool_id: tool_setting.to_dict()
            for tool_id, tool_setting in self._tool_settings.items()
        }
    
    def import_settings(self, settings_dict: Dict[str, Dict[str, Any]]):
        """导入工具设置（用于恢复备份）"""
        for tool_id, settings in settings_dict.items():
            tool_setting = self._tool_settings.get(tool_id)
            if tool_setting:
                tool_setting.from_dict(settings)
                self._save_tool_settings(tool_setting)
    
    # ========================================================================
    #  应用程序级别设置（整合自 ConfigManager）
    # ========================================================================
    
    def get_hotkey(self) -> str:
        """获取全局热键"""
        return self.qsettings.value("app/hotkey", self.APP_DEFAULT_SETTINGS["hotkey"], type=str)
    
    def set_hotkey(self, value: str):
        """设置全局热键"""
        self.qsettings.setValue("app/hotkey", value)
    
    def get_smart_selection(self) -> bool:
        """获取智能选区设置"""
        return self.qsettings.value("app/smart_selection", self.APP_DEFAULT_SETTINGS["smart_selection"], type=bool)
    
    def set_smart_selection(self, value: bool):
        """设置智能选区"""
        self.qsettings.setValue("app/smart_selection", value)
    
    def get_log_enabled(self) -> bool:
        """获取日志启用状态"""
        return self.qsettings.value("app/log_enabled", self.APP_DEFAULT_SETTINGS["log_enabled"], type=bool)
    
    def set_log_enabled(self, value: bool):
        """设置日志启用状态"""
        self.qsettings.setValue("app/log_enabled", value)
    
    def get_log_dir(self) -> str:
        """获取日志目录"""
        return self.qsettings.value("app/log_dir", self.APP_DEFAULT_SETTINGS["log_dir"], type=str)
    
    def set_log_dir(self, value: str):
        """设置日志目录"""
        self.qsettings.setValue("app/log_dir", value)
    
    def get_log_level(self) -> str:
        """获取日志等级 (DEBUG, INFO, WARNING, ERROR)"""
        return self.qsettings.value("app/log_level", self.APP_DEFAULT_SETTINGS["log_level"], type=str)
    
    def set_log_level(self, value: str):
        """设置日志等级 (DEBUG, INFO, WARNING, ERROR)"""
        self.qsettings.setValue("app/log_level", value)
    
    def get_log_retention_days(self) -> int:
        """获取日志保留天数（0表示永久保留）"""
        return self.qsettings.value("app/log_retention_days", self.APP_DEFAULT_SETTINGS["log_retention_days"], type=int)
    
    def set_log_retention_days(self, value: int):
        """设置日志保留天数（0表示永久保留）"""
        self.qsettings.setValue("app/log_retention_days", value)
    
    def get_long_stitch_engine(self) -> str:
        """获取长截图引擎"""
        return self.qsettings.value("app/long_stitch_engine", self.APP_DEFAULT_SETTINGS["long_stitch_engine"], type=str)
    
    def set_long_stitch_engine(self, value: str):
        """设置长截图引擎"""
        self.qsettings.setValue("app/long_stitch_engine", value)
    
    def get_scroll_cooldown(self) -> float:
        """获取滚动后等待时间（秒）"""
        return self.qsettings.value("screenshot/scroll_cooldown", self.APP_DEFAULT_SETTINGS["scroll_cooldown"], type=float)
    
    def set_scroll_cooldown(self, value: float):
        """设置滚动后等待时间（秒，0.05-1.0）"""
        self.qsettings.setValue("screenshot/scroll_cooldown", value)
    
    def get_screenshot_save_enabled(self) -> bool:
        """获取截图自动保存"""
        return self.qsettings.value("app/screenshot_save_enabled", self.APP_DEFAULT_SETTINGS["screenshot_save_enabled"], type=bool)
    
    def set_screenshot_save_enabled(self, value: bool):
        """设置截图自动保存"""
        self.qsettings.setValue("app/screenshot_save_enabled", value)
    
    def get_screenshot_save_path(self) -> str:
        """获取截图保存路径"""
        return self.qsettings.value("app/screenshot_save_path", self.APP_DEFAULT_SETTINGS["screenshot_save_path"], type=str)
    
    def set_screenshot_save_path(self, value: str):
        """设置截图保存路径"""
        self.qsettings.setValue("app/screenshot_save_path", value)
    
    def get_show_main_window(self) -> bool:
        """获取主窗口显示设置"""
        return self.qsettings.value("app/show_main_window", self.APP_DEFAULT_SETTINGS["show_main_window"], type=bool)
    
    def set_show_main_window(self, value: bool):
        """设置主窗口显示"""
        self.qsettings.setValue("app/show_main_window", value)
    
    def get_ocr_enabled(self) -> bool:
        """获取 OCR 启用状态"""
        return self.qsettings.value("app/ocr_enabled", self.APP_DEFAULT_SETTINGS["ocr_enabled"], type=bool)
    
    def set_ocr_enabled(self, value: bool):
        """设置 OCR 启用状态"""
        self.qsettings.setValue("app/ocr_enabled", value)
    
    def get_ocr_engine(self) -> str:
        """获取 OCR 引擎类型"""
        return self.qsettings.value("app/ocr_engine", self.APP_DEFAULT_SETTINGS["ocr_engine"], type=str)
    
    def set_ocr_engine(self, value: str):
        """设置 OCR 引擎类型"""
        self.qsettings.setValue("app/ocr_engine", value)
    
    def get_ocr_grayscale_enabled(self) -> bool:
        """获取 OCR 灰度化"""
        return self.qsettings.value("app/ocr_grayscale", self.APP_DEFAULT_SETTINGS["ocr_grayscale"], type=bool)
    
    # ==================== 钉图设置 ====================
    
    def get_pin_auto_toolbar(self) -> bool:
        """获取钉图是否自动显示工具栏"""
        return self.qsettings.value("pin/auto_toolbar", self.APP_DEFAULT_SETTINGS["pin_auto_toolbar"], type=bool)
    
    def set_pin_auto_toolbar(self, enabled: bool):
        """设置钉图自动显示工具栏"""
        self.qsettings.setValue("pin/auto_toolbar", enabled)
    
    def get_pin_default_opacity(self) -> float:
        """获取钉图默认透明度 (0.0-1.0)"""
        return self.qsettings.value("pin/default_opacity", self.APP_DEFAULT_SETTINGS["pin_default_opacity"], type=float)
    
    def set_pin_default_opacity(self, opacity: float):
        """设置钉图默认透明度"""
        self.qsettings.setValue("pin/default_opacity", max(0.1, min(1.0, opacity)))
    
    def set_ocr_grayscale_enabled(self, value: bool):
        """设置 OCR 灰度化"""
        self.qsettings.setValue("app/ocr_grayscale", value)
    
    def get_ocr_upscale_enabled(self) -> bool:
        """获取 OCR 放大"""
        return self.qsettings.value("app/ocr_upscale", self.APP_DEFAULT_SETTINGS["ocr_upscale"], type=bool)
    
    def set_ocr_upscale_enabled(self, value: bool):
        """设置 OCR 放大"""
        self.qsettings.setValue("app/ocr_upscale", value)
    
    def get_ocr_upscale_factor(self) -> float:
        """获取 OCR 放大倍数"""
        return self.qsettings.value("app/ocr_upscale_factor", self.APP_DEFAULT_SETTINGS["ocr_upscale_factor"], type=float)
    
    def set_ocr_upscale_factor(self, value: float):
        """设置 OCR 放大倍数"""
        self.qsettings.setValue("app/ocr_upscale_factor", value)
    
    # ==================== 翻译设置 ====================
    
    def get_deepl_api_key(self) -> str:
        """获取 DeepL API 密钥"""
        return self.qsettings.value("app/deepl_api_key", self.APP_DEFAULT_SETTINGS["deepl_api_key"], type=str)
    
    def set_deepl_api_key(self, value: str):
        """设置 DeepL API 密钥"""
        self.qsettings.setValue("app/deepl_api_key", value)
    
    def get_deepl_use_pro(self) -> bool:
        """获取是否使用 DeepL Pro API"""
        return self.qsettings.value("app/deepl_use_pro", self.APP_DEFAULT_SETTINGS["deepl_use_pro"], type=bool)
    
    def set_deepl_use_pro(self, value: bool):
        """设置是否使用 DeepL Pro API"""
        self.qsettings.setValue("app/deepl_use_pro", value)
    
    def get_translation_target_lang(self) -> str:
        """
        获取翻译目标语言
        
        如果未设置或为空，则跟随系统语言
        """
        saved = self.qsettings.value("app/translation_target_lang", "", type=str)
        if not saved:
            # 跟随系统语言
            from core.i18n import I18nManager
            sys_lang = I18nManager.get_system_language()
            # 映射到 DeepL 语言代码
            lang_map = {
                "zh": "ZH",
                "ja": "JA", 
                "en": "EN",
            }
            return lang_map.get(sys_lang, "EN")
        return saved
    
    def set_translation_target_lang(self, value: str):
        """设置翻译目标语言"""
        self.qsettings.setValue("app/translation_target_lang", value)
    
    def get_translation_split_sentences(self) -> bool:
        """获取是否启用自动分句"""
        return self.qsettings.value("app/translation_split_sentences", self.APP_DEFAULT_SETTINGS["translation_split_sentences"], type=bool)
    
    def set_translation_split_sentences(self, value: bool):
        """设置是否启用自动分句"""
        self.qsettings.setValue("app/translation_split_sentences", value)
    
    def get_translation_preserve_formatting(self) -> bool:
        """获取是否保留格式"""
        return self.qsettings.value("app/translation_preserve_formatting", self.APP_DEFAULT_SETTINGS["translation_preserve_formatting"], type=bool)
    
    def set_translation_preserve_formatting(self, value: bool):
        """设置是否保留格式"""
        self.qsettings.setValue("app/translation_preserve_formatting", value)
    
    # ==================== 剪贴板设置 ====================
    
    def get_clipboard_hotkey(self) -> str:
        """获取剪贴板管理器快捷键"""
        return self.qsettings.value("clipboard/hotkey", self.APP_DEFAULT_SETTINGS["clipboard_hotkey"], type=str)
    
    def set_clipboard_hotkey(self, value: str):
        """设置剪贴板管理器快捷键"""
        self.qsettings.setValue("clipboard/hotkey", value)
    
    def get_clipboard_enabled(self) -> bool:
        """获取剪贴板监听是否启用"""
        return self.qsettings.value("clipboard/enabled", self.APP_DEFAULT_SETTINGS["clipboard_enabled"], type=bool)
    
    def set_clipboard_enabled(self, value: bool):
        """设置剪贴板监听是否启用"""
        self.qsettings.setValue("clipboard/enabled", value)
    
    def get_clipboard_auto_paste(self) -> bool:
        """获取是否自动粘贴"""
        return self.qsettings.value("clipboard/auto_paste", self.APP_DEFAULT_SETTINGS["clipboard_auto_paste"], type=bool)
    
    def set_clipboard_auto_paste(self, value: bool):
        """设置是否自动粘贴"""
        self.qsettings.setValue("clipboard/auto_paste", value)
    
    def get_clipboard_history_limit(self) -> int:
        """获取历史记录数量限制"""
        return self.qsettings.value("clipboard/history_limit", self.APP_DEFAULT_SETTINGS["clipboard_history_limit"], type=int)
    
    def set_clipboard_history_limit(self, value: int):
        """设置历史记录数量限制"""
        self.qsettings.setValue("clipboard/history_limit", max(0, value))
    
    def get_clipboard_auto_cleanup(self) -> bool:
        """获取是否自动清理超出限制的记录"""
        return self.qsettings.value("clipboard/auto_cleanup", self.APP_DEFAULT_SETTINGS["clipboard_auto_cleanup"], type=bool)
    
    def set_clipboard_auto_cleanup(self, value: bool):
        """设置是否自动清理超出限制的记录"""
        self.qsettings.setValue("clipboard/auto_cleanup", value)
    
    def get_clipboard_display_lines(self) -> int:
        """获取剪贴板项显示行数（1/2）"""
        return self.qsettings.value("clipboard/display_lines", self.APP_DEFAULT_SETTINGS["clipboard_display_lines"], type=int)
    
    def set_clipboard_display_lines(self, value: int):
        """设置剪贴板项显示行数（1/2）"""
        # 限制在 1-2 之间
        value = max(1, min(2, value))
        self.qsettings.setValue("clipboard/display_lines", value)
    
    def get_clipboard_window_opacity(self) -> int:
        """获取剪贴板窗口透明度（0=不透明，数值越大越透明）"""
        return self.qsettings.value("clipboard/window_opacity", self.APP_DEFAULT_SETTINGS["clipboard_window_opacity"], type=int)
    
    def set_clipboard_window_opacity(self, value: int):
        """设置剪贴板窗口透明度"""
        self.qsettings.setValue("clipboard/window_opacity", value)
    
    def get_clipboard_window_opacity_options(self) -> list:
        """获取剪贴板窗口透明度可选项列表"""
        return self.APP_DEFAULT_SETTINGS["clipboard_window_opacity_options"]
    
    def get_clipboard_line_height_padding(self) -> int:
        """获取多行显示时的额外行高边距（像素）"""
        return self.qsettings.value("clipboard/line_height_padding", 
                                    self.APP_DEFAULT_SETTINGS["clipboard_line_height_padding"], 
                                    type=int)
    
    def set_clipboard_line_height_padding(self, value: int):
        """设置多行显示时的额外行高边距（像素）"""
        self.qsettings.setValue("clipboard/line_height_padding", max(0, value))
    
    def get_clipboard_move_to_top_on_paste(self) -> bool:
        """获取粘贴后是否将内容移到最前（默认 True）"""
        return self.qsettings.value("clipboard/move_to_top_on_paste", True, type=bool)
    
    def set_clipboard_move_to_top_on_paste(self, value: bool):
        """设置粘贴后是否将内容移到最前"""
        self.qsettings.setValue("clipboard/move_to_top_on_paste", value)

    def is_first_run(self) -> bool:
        """
        检测是否首次运行
        
        逻辑：如果注册表中没有任何配置记录，说明是首次运行
        通过检查一个标记键 "app/has_run_before" 来判断
        """
        return not self.qsettings.value("app/has_run_before", False, type=bool)
    
    def mark_as_run(self):
        """
        标记程序已经运行过一次
        """
        self.qsettings.setValue("app/has_run_before", True)
        self.qsettings.sync()
    
    def should_show_main_window_on_start(self) -> bool:
        """
        判断启动时是否应该显示主窗口
        
        规则：
        1. 如果是首次运行 -> 显示（引导用户）
        2. 如果不是首次运行 -> 根据用户设置决定
        
        Returns:
            bool: True=显示主窗口，False=后台启动
        """
        # 首次运行：强制显示
        if self.is_first_run():
            print("✨ [启动] 检测到首次运行，将自动打开设置窗口")
            return True
        
        # 非首次运行：读取用户设置
        show = self.get_show_main_window()
        print(f"🚀 [启动] 根据用户设置：{'显示主窗口' if show else '后台启动'}")
        return show


# 全局单例
_tool_settings_manager = None


def get_tool_settings_manager() -> ToolSettingsManager:
    """
    获取全局工具设置管理器单例
    
    注意：这个管理器现在同时管理工具设置和应用设置
    虽然名字是 tool_settings_manager，但实际上是统一的配置管理器
    """
    global _tool_settings_manager
    if _tool_settings_manager is None:
        _tool_settings_manager = ToolSettingsManager()
    return _tool_settings_manager

