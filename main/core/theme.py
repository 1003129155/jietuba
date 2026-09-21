# -*- coding: utf-8 -*-
"""
主题管理器 — 集中管理全局主题色和选区外观

所有截图相关组件（选区框、工具栏、放大镜、信息面板等）
统一从此模块获取颜色和选区外观参数，方便一处修改全局生效。

默认值定义在 settings/tool_settings.py 的 APP_DEFAULT_SETTINGS 中，
本模块通过 config_manager.get_app_setting / set_app_setting 读写。
"""
from PySide6.QtGui import QColor


class ThemeManager:
    """全局主题管理器（单例）"""

    # 选区边框笔宽可选档位（物理像素）。1 是「只要一条细线」，再细就看不见了
    BORDER_WIDTH_OPTIONS = (1, 2, 3, 4, 5, 6, 8, 10)
    DEFAULT_BORDER_WIDTH = 4

    # 选区手柄显示档位
    HANDLES_ALL = "all"          # 四角 + 四边，共八个
    HANDLES_CORNERS = "corners"  # 只画四角
    HANDLES_NONE = "none"        # 一个不画
    HANDLE_STYLE_OPTIONS = (HANDLES_ALL, HANDLES_CORNERS, HANDLES_NONE)

    # 选区手柄大小档位：圆点直径和外圈描边宽度是一体的视觉比例，不拆成两个设置项，
    # 只给"小/中/大"三档，用户凭观感选，不需要知道具体像素值
    HANDLE_SIZE_SMALL = "small"
    HANDLE_SIZE_MEDIUM = "medium"
    HANDLE_SIZE_LARGE = "large"
    HANDLE_SIZE_OPTIONS = (HANDLE_SIZE_SMALL, HANDLE_SIZE_MEDIUM, HANDLE_SIZE_LARGE)
    DEFAULT_HANDLE_SIZE = HANDLE_SIZE_SMALL
    # 档位 -> (圆点直径, 外圈描边宽度)，物理像素。medium 是重构前的历史硬编码值
    _HANDLE_SIZE_TABLE = {
        HANDLE_SIZE_SMALL: (10, 2),
        HANDLE_SIZE_MEDIUM: (14, 3),
        HANDLE_SIZE_LARGE: (18, 4),
    }

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
            cls._instance._selection_border_width = cls.DEFAULT_BORDER_WIDTH
            cls._instance._selection_handle_style = cls.HANDLES_ALL
            cls._instance._selection_handle_size = cls.DEFAULT_HANDLE_SIZE
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

        width = self._config_manager.get_app_setting("selection_border_width")
        if width is not None:
            self._selection_border_width = self._clamp_border_width(width)

        style = self._config_manager.get_app_setting("selection_handle_style")
        if style in self.HANDLE_STYLE_OPTIONS:
            self._selection_handle_style = style

        size = self._config_manager.get_app_setting("selection_handle_size")
        if size in self.HANDLE_SIZE_OPTIONS:
            self._selection_handle_size = size

    @classmethod
    def _clamp_border_width(cls, value) -> int:
        """把任意来源的笔宽收进合法区间。配置文件是可以手改的，读进来就归一化一次。"""
        try:
            width = int(value)
        except (TypeError, ValueError):
            return cls.DEFAULT_BORDER_WIDTH
        return max(cls.BORDER_WIDTH_OPTIONS[0], min(cls.BORDER_WIDTH_OPTIONS[-1], width))

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

    @property
    def selection_border_width(self) -> int:
        """选区边框笔宽（物理像素）"""
        return self._selection_border_width

    @property
    def selection_handle_style(self) -> str:
        """选区手柄显示档位，取值见 HANDLE_STYLE_OPTIONS"""
        return self._selection_handle_style

    @property
    def selection_handle_size(self) -> str:
        """选区手柄大小档位，取值见 HANDLE_SIZE_OPTIONS"""
        return self._selection_handle_size

    @property
    def selection_handle_diameter(self) -> int:
        """选区手柄圆点直径（物理像素），随大小档位一起变"""
        return self._HANDLE_SIZE_TABLE[self._selection_handle_size][0]

    @property
    def selection_handle_ring_width(self) -> int:
        """选区手柄白色外圈笔宽（物理像素），随大小档位一起变"""
        return self._HANDLE_SIZE_TABLE[self._selection_handle_size][1]

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

    def set_selection_border_width(self, width: int):
        """设置选区边框笔宽并持久化"""
        self._selection_border_width = self._clamp_border_width(width)
        if self._config_manager:
            self._config_manager.set_app_setting(
                "selection_border_width", self._selection_border_width
            )

    def set_selection_handle_style(self, style: str):
        """设置选区手柄显示档位并持久化；不认识的取值退回八个手柄"""
        if style not in self.HANDLE_STYLE_OPTIONS:
            style = self.HANDLES_ALL
        self._selection_handle_style = style
        if self._config_manager:
            self._config_manager.set_app_setting("selection_handle_style", style)

    def set_selection_handle_size(self, size: str):
        """设置选区手柄大小档位并持久化；不认识的取值退回默认档"""
        if size not in self.HANDLE_SIZE_OPTIONS:
            size = self.DEFAULT_HANDLE_SIZE
        self._selection_handle_size = size
        if self._config_manager:
            self._config_manager.set_app_setting("selection_handle_size", size)

    # ================================================================
    # 重置
    # ================================================================

    def reset_to_defaults(self):
        """恢复所有外观设置到默认值"""
        self.set_theme_color(QColor("#40E0D0"))
        self.set_mask_color(QColor(0, 0, 0, 120))
        self.set_selection_border_width(self.DEFAULT_BORDER_WIDTH)
        self.set_selection_handle_style(self.HANDLES_ALL)
        self.set_selection_handle_size(self.DEFAULT_HANDLE_SIZE)


def get_theme() -> ThemeManager:
    """获取全局主题管理器单例"""
    return ThemeManager()


def contrast_ink(background: QColor) -> QColor:
    """压在 background 上的图样、文字该用深色还是白色。

    主题色是用户可改的，按亮度取黑或白，保证任何主题色上都看得清。
    """
    luminance = 0.299 * background.red() + 0.587 * background.green() + 0.114 * background.blue()
    return QColor(20, 20, 20) if luminance > 150 else QColor(255, 255, 255)
 