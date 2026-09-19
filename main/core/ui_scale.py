# -*- coding: utf-8 -*-
"""
操作界面缩放管理器 —— 集中管理工具栏、二级面板、弹层一类浮层的显示比例

只作用于操作界面尺寸（按钮、图标、字号、间距、边距、圆角、自绘控件的点击区域）。
截图像素尺寸、绘制线宽、文字工具实际字号、录制区域这些内容数据不受影响。

尺寸一律用 Qt 逻辑像素，系统 DPI 缩放仍交给 Qt 处理。

各组件把自己的基准尺寸写成常量，每次都从基准重新算 scaled(基准)，
不要在已缩放的值上再乘比例 —— 反复切换比例会累积舍入误差。

默认值定义在 settings/tool_settings.py 的 APP_DEFAULT_SETTINGS["ui_scale_percent"]，
本模块通过 config_manager.get_app_setting / set_app_setting 读写。
"""
from PySide6.QtCore import QObject, Signal


class _ScaleSignals(QObject):
    """承载缩放变更信号的 QObject（管理器本身是普通单例，不继承 QObject）"""
    # 不带参数：接收方直接连自己的 apply_scale()，需要比例时问 get_ui_scale()
    scale_changed = Signal()


class UIScaleManager:
    """操作界面缩放管理器（单例）"""

    # 设置界面提供的档位，存储时用整数百分比，避免浮点在 QSettings 里往返失真
    PERCENT_OPTIONS = (80, 90, 100, 110, 125, 150)
    DEFAULT_PERCENT = 100

    _instance = None

    def __init__(self):
        if hasattr(self, "_initialized"):
            return
        self._initialized = True
        self._signals = _ScaleSignals()
        self._config_manager = None
        self._percent = self.DEFAULT_PERCENT

    @classmethod
    def instance(cls) -> "UIScaleManager":
        if cls._instance is None:
            cls._instance = cls()
        return cls._instance

    # ================================================================
    # 初始化（从配置加载）
    # ================================================================

    def init(self, config_manager):
        """绑定配置管理器并载入已保存的比例"""
        self._config_manager = config_manager
        saved = config_manager.get_app_setting("ui_scale_percent")
        self._percent = self.normalize_percent(saved)

    # ================================================================
    # 取值
    # ================================================================

    @property
    def scale_changed(self):
        """比例变更信号。接上自己的 apply_scale()，改比例时会被调用。"""
        return self._signals.scale_changed

    @property
    def percent(self) -> int:
        return self._percent

    @property
    def factor(self) -> float:
        return self._percent / 100.0

    def px(self, base) -> int:
        """把基准像素换算成当前比例下的整数像素。

        基准 0 保持 0（间距/边距可以就是 0），其余至少 1px，
        免得分隔线一类 1px 的元素在缩小档位里消失。
        """
        if not base:
            return 0
        value = round(base * self.factor)
        return max(1, int(value)) if base > 0 else min(-1, int(value))

    def pxf(self, base) -> float:
        """浮点版本，供描边宽度、圆角半径这类需要亚像素精度的绘制使用"""
        return float(base) * self.factor

    # ================================================================
    # 设置（同时持久化）
    # ================================================================

    def set_percent(self, percent) -> bool:
        """设置并持久化比例，发生变化时发出 scale_changed。返回是否真的变了。"""
        percent = self.normalize_percent(percent)
        changed = percent != self._percent
        self._percent = percent
        if self._config_manager:
            self._config_manager.set_app_setting("ui_scale_percent", percent)
        if changed:
            self._signals.scale_changed.emit()
        return changed

    @classmethod
    def normalize_percent(cls, value) -> int:
        """把任意输入收敛到最近的合法档位，非法值回落到默认值"""
        try:
            value = int(round(float(value)))
        except (TypeError, ValueError):
            return cls.DEFAULT_PERCENT
        return min(cls.PERCENT_OPTIONS, key=lambda option: (abs(option - value), option))

    def reset_to_default(self):
        self.set_percent(self.DEFAULT_PERCENT)


def get_ui_scale() -> UIScaleManager:
    """获取全局缩放管理器单例"""
    return UIScaleManager.instance()


def scaled(base) -> int:
    """基准像素 → 当前比例下的整数像素"""
    return UIScaleManager.instance().px(base)


def scaled_f(base) -> float:
    """基准像素 → 当前比例下的浮点像素（描边、圆角等）"""
    return UIScaleManager.instance().pxf(base)


def scale_factor() -> float:
    """当前比例因子（1.0 = 100%）"""
    return UIScaleManager.instance().factor
