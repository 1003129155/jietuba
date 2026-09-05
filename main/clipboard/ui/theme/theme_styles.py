# -*- coding: utf-8 -*-
"""
主题样式生成器

将主题颜色转换为 Qt StyleSheet
"""

from .themes import Theme


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
    
    def generate_time_filter_bar_style(self, opacity: int = 0) -> str:
        """Generate the expandable time filter bar style."""
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

    def generate_filter_toggle_btn_style(self) -> str:
        """Generate the time filter disclosure button style."""
        return f"""
            QToolButton {{
                background: transparent;
                color: {self.colors.text_secondary};
                border: none;
                border-radius: 4px;
                font-size: 17px;
                padding: 0px;
            }}
            QToolButton:hover {{
                background: {self.colors.bg_hover};
                color: {self.colors.text_primary};
            }}
            QToolButton:checked {{
                background: {self.colors.bg_hover};
                color: {self.colors.text_primary};
            }}
        """

    def generate_time_filter_date_edit_style(self) -> str:
        """Generate the date range filter edit style."""
        return f"""
            QDateEdit {{
                background: {self.colors.bg_primary};
                color: {self.colors.text_primary};
                border: 1px solid {self.colors.border_primary};
                border-radius: 4px;
                padding: 4px 8px;
                font-size: 12px;
            }}
            QDateEdit:focus {{
                border-color: {self.colors.border_accent};
            }}
            QDateEdit[hasError="true"] {{
                border-color: {self.colors.error};
                background: {self.colors.error_bg};
            }}
            QDateEdit::drop-down {{
                subcontrol-origin: padding;
                subcontrol-position: top right;
                width: 18px;
                border-left: 1px solid {self.colors.border_primary};
                border-top-right-radius: 4px;
                border-bottom-right-radius: 4px;
                background: {self.colors.bg_tertiary};
            }}
            QDateEdit::down-arrow {{
                image: none;
                width: 0px;
                height: 0px;
                border-left: 4px solid transparent;
                border-right: 4px solid transparent;
                border-top: 5px solid {self.colors.text_secondary};
            }}
        """

    def generate_time_filter_calendar_style(self) -> str:
        """Generate the popup calendar style used by date range filters."""
        return f"""
            QCalendarWidget {{
                background: {self.colors.bg_primary};
                color: {self.colors.text_primary};
                border: 1px solid {self.colors.border_primary};
            }}
            QCalendarWidget QWidget {{
                background: {self.colors.bg_primary};
                color: {self.colors.text_primary};
                alternate-background-color: {self.colors.bg_secondary};
            }}
            QCalendarWidget QToolButton {{
                background: transparent;
                color: {self.colors.text_primary};
                border: none;
                border-radius: 4px;
                padding: 4px;
            }}
            QCalendarWidget QToolButton:hover {{
                background: {self.colors.bg_hover};
            }}
            QCalendarWidget QMenu {{
                background: {self.colors.bg_primary};
                color: {self.colors.text_primary};
                border: 1px solid {self.colors.border_primary};
            }}
            QCalendarWidget QMenu::item:selected {{
                background: {self.colors.bg_selected};
                color: {self.colors.text_primary};
            }}
            QCalendarWidget QSpinBox {{
                background: {self.colors.bg_primary};
                color: {self.colors.text_primary};
                border: 1px solid {self.colors.border_primary};
                border-radius: 4px;
                padding: 2px 4px;
            }}
            QCalendarWidget QAbstractItemView {{
                background: {self.colors.bg_primary};
                color: {self.colors.text_primary};
                selection-background-color: {self.colors.accent_primary};
                selection-color: white;
                outline: none;
            }}
            QCalendarWidget QAbstractItemView:disabled {{
                color: {self.colors.text_disabled};
            }}
        """

    def generate_time_filter_action_btn_style(self, primary: bool = False) -> str:
        """Generate the time filter action button style."""
        bg = self.colors.accent_primary if primary else "transparent"
        fg = "white" if primary else self.colors.text_secondary
        border = self.colors.accent_primary if primary else self.colors.border_primary
        hover_bg = self.colors.accent_hover if primary else self.colors.bg_hover
        hover_fg = "white" if primary else self.colors.text_primary
        if primary and self.theme.name == "dark":
            bg = "#4E6578"
            border = "#4E6578"
            hover_bg = "#5C7488"

        return f"""
            QToolButton {{
                background: {bg};
                color: {fg};
                border: 1px solid {border};
                border-radius: 4px;
                padding: 0px;
                font-size: 12px;
                font-weight: 500;
            }}
            QToolButton:hover {{
                background: {hover_bg};
                color: {hover_fg};
            }}
        """

    def generate_time_filter_combo_style(self) -> str:
        """Generate the compact content-type filter combo style."""
        return f"""
            QComboBox {{
                background: {self.colors.bg_primary};
                color: {self.colors.text_primary};
                border: 1px solid {self.colors.border_primary};
                border-radius: 4px;
                padding: 3px 12px 3px 8px;
                font-size: 12px;
            }}
            QComboBox:hover {{
                background: {self.colors.bg_hover};
            }}
            QComboBox::drop-down {{
                subcontrol-origin: padding;
                subcontrol-position: top right;
                width: 10px;
                border-left: none;
                border-top-right-radius: 4px;
                border-bottom-right-radius: 4px;
                background: {self.colors.bg_tertiary};
            }}
            QComboBox::down-arrow {{
                image: none;
                width: 0px;
                height: 0px;
                border-left: 3px solid transparent;
                border-right: 3px solid transparent;
                border-top: 4px solid {self.colors.text_secondary};
            }}
            QComboBox QAbstractItemView {{
                background: {self.colors.bg_primary};
                color: {self.colors.text_primary};
                border: 1px solid {self.colors.border_primary};
                selection-background-color: {self.colors.accent_primary};
                selection-color: white;
                outline: none;
            }}
        """

    def generate_time_filter_type_btn_style(self) -> str:
        """Generate the custom content-type dropdown button style."""
        return f"""
            QToolButton {{
                background: {self.colors.bg_primary};
                color: {self.colors.text_primary};
                border: 1px solid {self.colors.border_primary};
                border-radius: 4px;
                padding: 3px 6px;
                font-size: 12px;
                text-align: center;
            }}
            QToolButton:hover {{
                background: {self.colors.bg_hover};
            }}
            QToolButton:pressed {{
                background: {self.colors.bg_selected};
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
