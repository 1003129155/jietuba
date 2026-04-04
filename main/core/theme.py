# -*- coding: utf-8 -*-
"""
主题颜色管理器 — 集中管理全局主题色和可自定义颜色

所有截图相关组件（选区框、工具栏、放大镜、信息面板等）
统一从此模块获取颜色，方便一处修改全局生效。

默认值定义在 settings/tool_settings.py 的 APP_DEFAULT_SETTINGS 中，
本模块通过 config_manager.get_app_setting / set_app_setting 读写。
"""
from PySide6.QtGui import QColor


class ThemeManager:
    """全局主题颜色管理器（单例）"""

    _instance = None
    _config_manager = None

    # ── 缓存的 QColor 对象 ──
    _theme_color: QColor = None
    _mask_color: QColor = None

    def __new__(cls):
        if cls._instance is None:
            cls._instance = super().__new__(cls)
            # 硬编码的回退默认值（正常流程会被 init() 覆盖）
            cls._instance._theme_color = QColor("#40E0D0")
            cls._instance._mask_color = QColor(0, 0, 0, 120)
        return cls._instance

    # ================================================================
    # 初始化（从配置加载）
    # ================================================================

    def init(self, config_manager):
        """绑定配置管理器并从持久化设置加载颜色"""
        self._config_manager = config_manager
        self._load_from_settings()

    def _load_from_settings(self):
        """从 config_manager (APP_DEFAULT_SETTINGS) 加载保存的颜色"""
        if not self._config_manager:
            return

        # 主题色 — 存储为 "#RRGGBB" hex 字符串
        saved_hex = self._config_manager.get_app_setting("theme_color")
        if saved_hex:
            c = QColor(saved_hex)
            if c.isValid():
                self._theme_color = c

        # 遮罩色 — 分 R/G/B 三个整数存储，Alpha 固定 120
        r = self._config_manager.get_app_setting("mask_color_r")
        g = self._config_manager.get_app_setting("mask_color_g")
        b = self._config_manager.get_app_setting("mask_color_b")
        if r is not None and g is not None and b is not None:
            self._mask_color = QColor(int(r), int(g), int(b), 120)

    # ================================================================
    # Getters
    # ================================================================

    @property
    def theme_color(self) -> QColor:
        """主题色（用于选区框、工具栏描边、放大镜十字线等）"""
        return QColor(self._theme_color)

    @property
    def theme_color_hex(self) -> str:
        """主题色 hex 字符串（不含 alpha），如 '#40E0D0'"""
        return self._theme_color.name().upper()

    @property
    def mask_color(self) -> QColor:
        """截图遮罩色"""
        return QColor(self._mask_color)

    # ================================================================
    # Setters（同时持久化）
    # ================================================================

    def set_theme_color(self, color: QColor):
        """设置主题色并持久化"""
        self._theme_color = QColor(color)
        if self._config_manager:
            self._config_manager.set_app_setting("theme_color", color.name())

    def set_mask_color(self, color: QColor):
        """设置遮罩色并持久化（只存 RGB，Alpha 固定 120）"""
        self._mask_color = QColor(color.red(), color.green(), color.blue(), 120)
        if self._config_manager:
            self._config_manager.set_app_setting("mask_color_r", color.red())
            self._config_manager.set_app_setting("mask_color_g", color.green())
            self._config_manager.set_app_setting("mask_color_b", color.blue())

    # ================================================================
    # 重置
    # ================================================================

    def reset_to_defaults(self):
        """恢复所有颜色到默认值"""
        self.set_theme_color(QColor("#40E0D0"))
        self.set_mask_color(QColor(0, 0, 0, 120))


def get_theme() -> ThemeManager:
    """获取全局主题管理器单例"""
    return ThemeManager()
 