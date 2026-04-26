# -*- coding: utf-8 -*-
"""
主题样式生成器

将主题颜色转换为 Qt StyleSheet
"""

from typing import Optional
from .themes import Theme, ThemeColors


class ThemeStyleGenerator:
    """主题样式生成器"""
    
    def __init__(self, theme: Theme):
        self.theme = theme
        self.colors = theme.colors
    
    def generate_window_style(self, opacity: int = 0) -> str:
        """
        生成主窗口样式
        
        Args:
            opacity: 透明度 (0-100, 0=不透明)
        """
        alpha = 255 - int(opacity * 2.55)
        
        return f"""
            QFrame#mainContainer {{
                background: rgba({self._hex_to_rgb(self.colors.bg_primary)}, {alpha / 255});
                border: 1px solid {self.colors.border_primary};
                border-radius: 4px;
            }}
            QToolTip {{
                background: {self.colors.bg_primary};
                color: {self.colors.text_primary};
                border: 1px solid {self.colors.border_primary};
                border-radius: 4px;
                padding: 4px 8px;
                font-size: 12px;
            }}
        """
    
    def generate_list_widget_style(self, opacity: int = 0) -> str:
        """生成列表控件样式"""
        alpha = 255 - int(opacity * 2.55)
        
        return f"""
            QListWidget {{
                background: rgba({self._hex_to_rgb(self.colors.bg_primary)}, {alpha / 255});
                border: none;
                outline: none;
                color: {self.colors.text_primary};
            }}
            QListWidget::item {{
                padding: 0px;
                border: none;
                background: transparent;
            }}
            QListWidget::item:selected {{
                background: transparent;
            }}
            QListWidget::item:hover {{
                background: transparent;
            }}
        """
    
    def generate_search_bar_style(self, opacity: int = 0) -> str:
        """生成搜索栏样式"""
        alpha = 255 - int(opacity * 2.55)
        
        return f"""
            QWidget {{
                background: rgba({self._hex_to_rgb(self.colors.bg_secondary)}, {alpha / 255});
                border-top: 1px solid {self.colors.border_primary};
            }}
        """
    
    def generate_button_style(self, button_type: str = "normal") -> str:
        """
        生成按钮样式
        
        Args:
            button_type: 按钮类型 (normal, primary, danger)
        """
        if button_type == "primary":
            return f"""
                QPushButton {{
                    background: {self.colors.accent_primary};
                    color: white;
                    border: none;
                    border-radius: 6px;
                    padding: 10px 24px;
                    font-size: 13px;
                    font-weight: 500;
                }}
                QPushButton:hover {{
                    background: {self.colors.accent_hover};
                }}
                QPushButton:pressed {{
                    background: {self.colors.accent_hover};
                }}
            """
        elif button_type == "danger":
            return f"""
                QPushButton {{
                    background: {self.colors.error_bg};
                    color: {self.colors.error};
                    border: none;
                    border-radius: 6px;
                    padding: 10px 24px;
                    font-size: 13px;
                }}
                QPushButton:hover {{
                    background: {self.colors.error_hover};
                }}
            """
        else:  # normal
            return f"""
                QPushButton {{
                    background: {self.colors.bg_secondary};
                    color: {self.colors.text_primary};
                    border: 1px solid {self.colors.border_primary};
                    border-radius: 6px;
                    padding: 8px 16px;
                    font-size: 13px;
                }}
                QPushButton:hover {{
                    background: {self.colors.bg_hover};
                }}
            """
    
    def generate_input_style(self) -> str:
        """生成输入框样式"""
        return f"""
            QLineEdit {{
                border: 1px solid {self.colors.border_primary};
                border-radius: 6px;
                padding: 8px 12px;
                font-size: 13px;
                background: {self.colors.bg_secondary};
                color: {self.colors.text_primary};
            }}
            QLineEdit:focus {{
                border-color: {self.colors.border_accent};
                background: {self.colors.bg_primary};
            }}
            QTextEdit {{
                border: 1px solid {self.colors.border_primary};
                border-radius: 6px;
                padding: 8px;
                font-size: 13px;
                background: {self.colors.bg_secondary};
                color: {self.colors.text_primary};
            }}
            QTextEdit:focus {{
                border-color: {self.colors.border_accent};
                background: {self.colors.bg_primary};
            }}
        """
    
    def generate_combobox_style(self) -> str:
        """生成下拉框样式"""
        return f"""
            QComboBox {{
                border: 1px solid {self.colors.border_primary};
                border-radius: 6px;
                padding: 6px 10px;
                font-size: 12px;
                background: {self.colors.bg_primary};
                color: {self.colors.text_primary};
            }}
            QComboBox QAbstractItemView {{
                background: {self.colors.bg_primary};
                color: {self.colors.text_primary};
                selection-background-color: {self.colors.bg_selected};
                selection-color: {self.colors.text_accent};
            }}
        """
    
    def generate_dialog_style(self) -> str:
        """生成对话框样式"""
        return f"""
            QDialog {{
                background: {self.colors.bg_primary};
            }}
        """
    
    def generate_manage_dialog_style(self) -> str:
        """生成管理对话框完整样式"""
        return f"""
            QDialog {{ background: {self.colors.bg_primary}; }}
            
            /* 输入框样式 */
            {self.generate_input_style()}
            
            /* 导航列样式 */
            QWidget#navColumn {{
                background: {self.colors.bg_tertiary};
                border-right: 1px solid {self.colors.border_secondary};
            }}
            
            /* 列表列样式 */
            QWidget#listColumn {{
                background: {self.colors.bg_secondary};
                border-right: 1px solid {self.colors.border_secondary};
            }}
            
            QListWidget {{
                background: transparent;
                border: none;
                outline: none;
                color: {self.colors.text_primary};
            }}
            QListWidget::item {{
                padding: 0px 12px;
                border-bottom: 1px solid {self.colors.bg_hover};
                color: {self.colors.text_primary};
            }}
            QListWidget::item:selected {{
                background: {self.colors.bg_selected};
                color: {self.colors.text_accent};
            }}
            QListWidget::item:hover {{
                background: {self.colors.bg_hover};
            }}
            
            /* 详情列样式 */
            QWidget#detailColumn {{
                background: {self.colors.bg_primary};
            }}
        """
    
    def generate_preview_popup_style(self) -> str:
        """生成预览弹窗样式"""
        return f"""
            PreviewPopup {{
                background: {self.colors.bg_secondary};
                border: 1px solid {self.colors.border_secondary};
                border-radius: 8px;
            }}
            QTextEdit {{
                background: {self.colors.bg_secondary};
                border: 1px solid {self.colors.border_primary};
                border-radius: 4px;
                padding: 4px 8px;
                font-size: 13px;
                color: {self.colors.text_primary};
            }}
        """

    def generate_menu_style(self) -> str:
        """生成右键菜单样式"""
        return f"""
            QMenu {{
                background: {self.colors.bg_primary};
                border: 1px solid {self.colors.border_primary};
                border-radius: 4px;
                padding: 4px;
            }}
            QMenu::item {{
                padding: 8px 20px;
                color: {self.colors.text_primary};
            }}
            QMenu::item:selected {{
                background: {self.colors.bg_hover};
            }}
            QMenu::separator {{
                height: 1px;
                background: {self.colors.border_primary};
                margin: 4px 8px;
            }}
        """

    def generate_search_input_style(self, has_text: bool = False) -> str:
        """生成搜索框样式"""
        bg = self.colors.bg_hover if has_text else "transparent"
        return f"""
            QLineEdit {{
                background: {bg};
                border: none;
                color: {self.colors.text_primary};
                font-size: 13px;
                padding: 4px;
            }}
        """

    def generate_clear_search_btn_style(self) -> str:
        """生成清除搜索按钮样式"""
        return f"""
            QPushButton {{
                background: transparent;
                color: {self.colors.text_tertiary};
                border: none;
                font-size: 16px;
                padding: 0px;
            }}
            QPushButton:hover {{
                background: {self.colors.bg_hover};
                color: {self.colors.text_primary};
                border-radius: 12px;
            }}
        """

    def generate_menu_btn_style(self) -> str:
        """生成齿轮菜单按钮样式"""
        return f"""
            QPushButton {{
                background: transparent;
                color: {self.colors.text_secondary};
                border: none;
                font-size: 18px;
                font-weight: normal;
            }}
            QPushButton:hover {{
                background: {self.colors.bg_hover};
                border-radius: 4px;
            }}
        """
    
    @staticmethod
    def _hex_to_rgb(hex_color: str) -> str:
        """
        将十六进制颜色转换为 RGB 字符串
        
        Args:
            hex_color: 十六进制颜色，如 "#FFFFFF"
            
        Returns:
            RGB 字符串，如 "255, 255, 255"
        """
        hex_color = hex_color.lstrip('#')
        if len(hex_color) == 3:
            hex_color = ''.join([c*2 for c in hex_color])
        
        r = int(hex_color[0:2], 16)
        g = int(hex_color[2:4], 16)
        b = int(hex_color[4:6], 16)
        
        return f"{r}, {g}, {b}"


def generate_all_styles(theme: Theme, opacity: int = 0) -> dict:
    """
    生成所有样式
    
    Args:
        theme: 主题对象
        opacity: 透明度 (0-100)
        
    Returns:
        包含所有样式的字典
    """
    generator = ThemeStyleGenerator(theme)
    
    return {
        "window": generator.generate_window_style(opacity),
        "list_widget": generator.generate_list_widget_style(opacity),
        "search_bar": generator.generate_search_bar_style(opacity),
        "button_normal": generator.generate_button_style("normal"),
        "button_primary": generator.generate_button_style("primary"),
        "button_danger": generator.generate_button_style("danger"),
        "input": generator.generate_input_style(),
        "combobox": generator.generate_combobox_style(),
        "dialog": generator.generate_dialog_style(),
        "manage_dialog": generator.generate_manage_dialog_style(),
        "preview_popup": generator.generate_preview_popup_style(),
    }
 