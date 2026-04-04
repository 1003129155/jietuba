# -*- coding: utf-8 -*-
"""
剪贴板主题系统

提供多种预设主题和自定义主题功能。
"""

from typing import Dict, Any, Optional
from dataclasses import dataclass, asdict
from PySide6.QtCore import QObject, Signal

from core.logger import log_exception


@dataclass
class ThemeColors:
    """主题颜色定义"""
    # 背景色
    bg_primary: str = "#FFFFFF"           # 主背景色
    bg_secondary: str = "#E0EAF0"         # 次要背景色
    bg_tertiary: str = "#F5F6F8"          # 第三级背景色
    bg_hover: str = "#F0F0F0"             # 悬停背景色
    bg_selected: str = "#B9D9F1"          # 选中背景色
    bg_selected_highlight: str = "rgba(0, 0, 0, 0.15)"  # 选中高亮背景
    
    # 文字色
    text_primary: str = "#333333"         # 主文字色
    text_secondary: str = "#666666"       # 次要文字色
    text_tertiary: str = "#999999"        # 第三级文字色
    text_disabled: str = "#CCCCCC"        # 禁用文字色
    text_accent: str = "#1976D2"          # 强调文字色
    
    # 边框色
    border_primary: str = "#E0E0E0"       # 主边框色
    border_secondary: str = "#E8E8E8"     # 次要边框色
    border_accent: str = "#1976D2"        # 强调边框色
    border_selected: str = "#0078D7"      # 选中边框色
    
    # 功能色
    accent_primary: str = "#1976D2"       # 主强调色（蓝色）
    accent_hover: str = "#1565C0"         # 强调色悬停
    success: str = "#4CAF50"              # 成功色（绿色）
    success_bg: str = "#E8F5E9"           # 成功背景色
    success_hover: str = "#388E3C"        # 成功色悬停
    error: str = "#F44336"                # 错误色（红色）
    error_bg: str = "#FFEBEE"             # 错误背景色
    error_hover: str = "#FFCDD2"          # 错误色悬停
    warning: str = "#FF9800"              # 警告色（橙色）
    warning_bg: str = "#FFF3E0"           # 警告背景色
    
    # 特殊色
    drag_drop_border: str = "#4DAF50"     # 拖放边框色
    separator: str = "#E0E0E0"            # 分隔线颜色
    shortcut_key_color: str = "#1976D2"   # 快捷键徽标文字颜色
    
    def to_dict(self) -> Dict[str, str]:
        """转换为字典"""
        return asdict(self)
    
    @classmethod
    def from_dict(cls, data: Dict[str, str]) -> 'ThemeColors':
        """从字典创建"""
        return cls(**data)


class Theme:
    """主题类"""
    
    def __init__(self, name: str, display_name: str, colors: ThemeColors, description: str = ""):
        self.name = name
        self.display_name = display_name
        self.colors = colors
        self.description = description
    
    def to_dict(self) -> Dict[str, Any]:
        """转换为字典"""
        return {
            "name": self.name,
            "display_name": self.display_name,
            "description": self.description,
            "colors": self.colors.to_dict()
        }
    
    @classmethod
    def from_dict(cls, data: Dict[str, Any]) -> 'Theme':
        """从字典创建"""
        return cls(
            name=data["name"],
            display_name=data["display_name"],
            colors=ThemeColors.from_dict(data["colors"]),
            description=data.get("description", "")
        )


# ==================== 预设主题 ====================

# 1. 默认浅色主题（Material Design）
THEME_LIGHT = Theme(
    name="light",
    display_name="浅色（默认）",
    description="Material Design 风格的浅色主题",
    colors=ThemeColors()  # 使用默认值
)

# 2. 深色主题
THEME_DARK = Theme(
    name="dark",
    display_name="深色",
    description="护眼深色主题，适合夜间使用",
    colors=ThemeColors(
        # 背景色
        bg_primary="#1E1E1E",
        bg_secondary="#3B3B3D",
        bg_tertiary="#2D2D30",
        bg_hover="#3E3E42",
        bg_selected="#0B4B75",
        bg_selected_highlight="rgba(0, 122, 204, 0.5)",  # 加深选中高亮
        
        # 文字色
        text_primary="#E0E0E0",
        text_secondary="#AAAAAA",
        text_tertiary="#888888",
        text_disabled="#555555",
        text_accent="#4FC3F7",
        
        # 边框色
        border_primary="#3E3E42",
        border_secondary="#2D2D30",
        border_accent="#007ACC",
        border_selected="#007ACC",
        
        # 功能色
        accent_primary="#007ACC",
        accent_hover="#005A9E",
        success="#4CAF50",
        success_bg="#1B5E20",
        success_hover="#388E3C",
        error="#F44336",
        error_bg="#5D1A1A",
        error_hover="#E53935",
        warning="#FFA726",
        warning_bg="#663C00",
        
        # 特殊色
        drag_drop_border="#4CAF50",
        separator="#3E3E42",
        shortcut_key_color="#9CA2C5",   # 浅蓝色，在深色背景上清晰可见
    )
)

# 3. 蓝色主题
THEME_BLUE = Theme(
    name="blue",
    display_name="海洋蓝",
    description="清新的蓝色系主题",
    colors=ThemeColors(
        bg_primary="#F0F8FF",
        bg_secondary="#D9EDFF",
        bg_tertiary="#D6EBFF",
        bg_hover="#CCE5FF",
        bg_selected="#7EC2FA",
        bg_selected_highlight="rgba(0, 0, 0, 0.20)",
        
        text_primary="#1A237E",
        text_secondary="#3949AB",
        text_tertiary="#5C6BC0",
        text_accent="#1976D2",
        
        border_primary="#BBDEFB",
        border_secondary="#C5CAE9",
        border_accent="#2196F3",
        border_selected="#1976D2",
        
        accent_primary="#2196F3",
        accent_hover="#1976D2",
        success="#4CAF50",
        success_bg="#C8E6C9",
        error="#F44336",
        error_bg="#FFCDD2",
        shortcut_key_color="#1565C0",   # 深蓝色，在浅蓝背景上醒目
    )
)

# 4. 绿色主题
THEME_GREEN = Theme(
    name="green",
    display_name="清新绿",
    description="自然清新的绿色主题",
    colors=ThemeColors(
        bg_primary="#F1F8F4",
        bg_secondary="#DBF7DD",
        bg_tertiary="#C8E6C9",
        bg_hover="#A5D6A7",
        bg_selected="#78CF7D",
        bg_selected_highlight="rgba(0, 0, 0, 0.20)",
        
        text_primary="#1B5E20",
        text_secondary="#388E3C",
        text_tertiary="#66BB6A",
        text_accent="#4CAF50",
        
        border_primary="#A5D6A7",
        border_secondary="#C8E6C9",
        border_accent="#4CAF50",
        border_selected="#388E3C",
        
        accent_primary="#4CAF50",
        accent_hover="#388E3C",
        success="#66BB6A",
        success_bg="#C8E6C9",
        error="#F44336",
        error_bg="#FFCDD2",
        shortcut_key_color="#1B5E20",   # 深绿色，在浅绿背景上醒目
    )
)

# 5. 粉色主题
THEME_PINK = Theme(
    name="pink",
    display_name="樱花粉",
    description="温柔的粉色系主题",
    colors=ThemeColors(
        bg_primary="#FFF0F5",
        bg_secondary="#FAD6E2",
        bg_tertiary="#F8BBD0",
        bg_hover="#F48FB1",
        bg_selected="#EE5A8C",
        bg_selected_highlight="rgba(203, 30, 99, 0.30)",
        
        text_primary="#880E4F",
        text_secondary="#C2185B",
        text_tertiary="#E91E63",
        text_accent="#E91E63",
        
        border_primary="#F8BBD0",
        border_secondary="#FCE4EC",
        border_accent="#E91E63",
        border_selected="#C2185B",
        
        accent_primary="#E91E63",
        accent_hover="#C2185B",
        success="#4CAF50",
        success_bg="#C8E6C9",
        error="#F44336",
        error_bg="#FFCDD2",
        shortcut_key_color="#880E4F",   # 深粉红，在浅粉背景上醒目
    )
)

# 6. 紫色主题
THEME_PURPLE = Theme(
    name="purple",
    display_name="优雅紫",
    description="优雅的紫色系主题",
    colors=ThemeColors(
        bg_primary="#F3E5F5",
        bg_secondary="#E4BCEB",
        bg_tertiary="#CE93D8",
        bg_hover="#BA68C8",
        bg_selected="#AD42C0",
        bg_selected_highlight="rgba(156, 39, 176, 0.50)",
        
        text_primary="#4A148C",
        text_secondary="#6A1B9A",
        text_tertiary="#8E24AA",
        text_accent="#9C27B0",
        
        border_primary="#CE93D8",
        border_secondary="#E1BEE7",
        border_accent="#9C27B0",
        border_selected="#7B1FA2",
        
        accent_primary="#9C27B0",
        accent_hover="#7B1FA2",
        success="#4CAF50",
        success_bg="#C8E6C9",
        error="#F44336",
        error_bg="#FFCDD2",
        shortcut_key_color="#4A148C",   # 深紫色，在浅紫背景上醒目
    )
)

# 7. 橙色主题
THEME_ORANGE = Theme(
    name="orange",
    display_name="活力橙",
    description="充满活力的橙色主题",
    colors=ThemeColors(
        bg_primary="#FFF3E0",
        bg_secondary="#FCD9A5",
        bg_tertiary="#FFCC80",
        bg_hover="#FFB74D",
        bg_selected="#FCA31F",
        bg_selected_highlight="rgba(255, 152, 0, 0.50)",
        
        text_primary="#E65100",
        text_secondary="#F57C00",
        text_tertiary="#FF9800",
        text_accent="#FF9800",
        
        border_primary="#FFCC80",
        border_secondary="#FFE0B2",
        border_accent="#FF9800",
        border_selected="#F57C00",
        
        accent_primary="#FF9800",
        accent_hover="#F57C00",
        success="#4CAF50",
        success_bg="#C8E6C9",
        error="#F44336",
        error_bg="#FFCDD2",
        shortcut_key_color="#BF360C",   # 深橙红，在浅橙背景上醒目
    )
)


# 所有预设主题
PRESET_THEMES = {
    "light": THEME_LIGHT,
    "dark": THEME_DARK,
    "blue": THEME_BLUE,
    "green": THEME_GREEN,
    "pink": THEME_PINK,
    "purple": THEME_PURPLE,
    "orange": THEME_ORANGE,
}


class ThemeManager(QObject):
    """主题管理器"""
    
    # 信号：主题改变时发出
    theme_changed = Signal(Theme)
    # 信号：字体大小改变时发出 (size: int)
    font_size_changed = Signal(int)
    # 信号：窗口透明度改变时发出 (percent: int)
    opacity_changed = Signal(int)
    
    def __init__(self):
        super().__init__()
        self._custom_themes: Dict[str, Theme] = {}
        # 从设置中加载保存的主题
        self._current_theme = self._load_saved_theme()
    
    def _load_saved_theme(self) -> Theme:
        """从设置中加载保存的主题"""
        try:
            from settings import get_tool_settings_manager
            config = get_tool_settings_manager()
            saved_theme_name = config.get_clipboard_theme()
            if saved_theme_name in PRESET_THEMES:
                return PRESET_THEMES[saved_theme_name]
        except Exception as e:
            log_exception(e, "加载保存的主题")
        return THEME_LIGHT  # 默认使用浅色主题
    
    def _save_theme(self, theme_name: str):
        """保存主题到设置"""
        try:
            from settings import get_tool_settings_manager
            config = get_tool_settings_manager()
            config.set_clipboard_theme(theme_name)
        except Exception as e:
            log_exception(e, "保存主题设置")
    
    def get_current_theme(self) -> Theme:
        """获取当前主题"""
        return self._current_theme
    
    def set_theme(self, theme_name: str) -> bool:
        """
        设置主题
        
        Args:
            theme_name: 主题名称
            
        Returns:
            bool: 是否成功
        """
        # 先从预设主题中查找
        if theme_name in PRESET_THEMES:
            self._current_theme = PRESET_THEMES[theme_name]
            self._save_theme(theme_name)  # 保存到设置
            self.theme_changed.emit(self._current_theme)
            return True
        
        # 再从自定义主题中查找
        if theme_name in self._custom_themes:
            self._current_theme = self._custom_themes[theme_name]
            self._save_theme(theme_name)  # 保存到设置
            self.theme_changed.emit(self._current_theme)
            return True
        
        return False
    
    def notify_font_size_changed(self, size: int):
        """通知字体大小变更（供外部设置页面调用）"""
        self.font_size_changed.emit(size)

    def notify_opacity_changed(self, percent: int):
        """通知窗口透明度变更（供外部设置页面调用）"""
        self.opacity_changed.emit(percent)

    def get_all_themes(self) -> Dict[str, Theme]:
        """获取所有可用主题"""
        all_themes = PRESET_THEMES.copy()
        all_themes.update(self._custom_themes)
        return all_themes
    
    def get_preset_themes(self) -> Dict[str, Theme]:
        """获取预设主题"""
        return PRESET_THEMES.copy()
    
    def add_custom_theme(self, theme: Theme) -> bool:
        """
        添加自定义主题
        
        Args:
            theme: 主题对象
            
        Returns:
            bool: 是否成功
        """
        if theme.name in PRESET_THEMES:
            return False  # 不能覆盖预设主题
        
        self._custom_themes[theme.name] = theme
        return True
    
    def remove_custom_theme(self, theme_name: str) -> bool:
        """删除自定义主题"""
        if theme_name in self._custom_themes:
            del self._custom_themes[theme_name]
            return True
        return False
    
    def export_theme(self, theme_name: str) -> Optional[Dict[str, Any]]:
        """导出主题为字典"""
        all_themes = self.get_all_themes()
        if theme_name in all_themes:
            return all_themes[theme_name].to_dict()
        return None
    
    def import_theme(self, theme_data: Dict[str, Any]) -> bool:
        """从字典导入主题"""
        try:
            theme = Theme.from_dict(theme_data)
            return self.add_custom_theme(theme)
        except Exception as e:
            log_exception(e, "导入主题")
            return False


# 全局主题管理器实例
_theme_manager: Optional[ThemeManager] = None


def get_theme_manager() -> ThemeManager:
    """获取全局主题管理器实例"""
    global _theme_manager
    if _theme_manager is None:
        _theme_manager = ThemeManager()
    return _theme_manager
 