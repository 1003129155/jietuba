# -*- coding: utf-8 -*-
"""
操作界面缩放管理器 —— 集中管理工具栏、二级面板、弹层一类浮层的显示比例

只作用于操作界面尺寸（按钮、图标、字号、间距、边距、圆角、自绘控件的点击区域）。
截图像素尺寸、绘制线宽、文字工具实际字号、录制区域这些内容数据不受影响。

尺寸一律用 Qt 像素；项目关闭了 Qt 自动高 DPI 缩放，首次运行时会根据
Windows 系统 DPI 为两套界面缩放选择一个最接近的初始档位。

各组件把自己的基准尺寸写成常量，每次都从基准重新算 scaled(基准)，
不要在已缩放的值上再乘比例 —— 反复切换比例会累积舍入误差。

默认值定义在 settings/tool_settings.py 的 APP_DEFAULT_SETTINGS["ui_scale_percent"]，
本模块通过 config_manager.get_app_setting / set_app_setting 读写。
"""
import math

from PySide6.QtCore import QObject, Signal
from PySide6.QtWidgets import QWidget


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


def recommended_scale_percent(system_dpi=None) -> int:
    """按系统缩放区间选择保守的首次运行档位（96 DPI = 100%）。

    系统缩放不超过 100% 时使用 100%；大于 100% 且不超过 150% 时
    使用 125%；超过 150% 时使用 150%。这样不会让中等高 DPI 屏幕上的
    固定尺寸窗口被等比例放得过大。
    """
    if system_dpi is None:
        try:
            from core.platform_utils import get_system_dpi
            system_dpi = get_system_dpi()
        except Exception:
            # 自动检测只能改善首次体验，绝不能因为系统 API 异常阻断启动。
            system_dpi = 96.0
    try:
        percent = float(system_dpi) / 96.0 * 100.0
    except (TypeError, ValueError, ZeroDivisionError):
        percent = UIScaleManager.DEFAULT_PERCENT
    if not math.isfinite(percent) or percent <= 0:
        percent = UIScaleManager.DEFAULT_PERCENT
    if percent > 150:
        return 150
    if percent > 100:
        return 125
    return 100


def apply_first_run_scale_defaults(config_manager, system_dpi=None):
    """首次运行时为尚未配置的两套缩放写入系统推荐值。

    已存在的任一设置都会保留，避免用户在向导期间重启，或预先导入配置后，
    再次启动时被自动检测覆盖。返回实际推荐档位；非首次运行返回 ``None``。
    """
    if not config_manager.is_first_run():
        return None

    recommended = recommended_scale_percent(system_dpi)
    settings = getattr(config_manager, "qsettings", None)
    for key in ("ui_scale_percent", "dialog_scale_percent"):
        setting_key = f"app/{key}"
        if settings is None or not settings.contains(setting_key):
            config_manager.set_app_setting(key, recommended)
    return recommended


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


class DialogScaleManager:
    """独立业务窗口的界面缩放管理器。

    与工具栏/面板的 UIScaleManager 分开：工具栏倍率管截图操作界面，
    这一档管日常窗口的字号与尺寸。结构与 UIScaleManager 一致。
    """

    PERCENT_OPTIONS = UIScaleManager.PERCENT_OPTIONS
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
    def instance(cls) -> "DialogScaleManager":
        if cls._instance is None:
            cls._instance = cls()
        return cls._instance

    def init(self, config_manager):
        """绑定配置管理器并载入已保存的比例"""
        self._config_manager = config_manager
        saved = config_manager.get_app_setting("dialog_scale_percent")
        self._percent = UIScaleManager.normalize_percent(saved)

    @property
    def scale_changed(self):
        return self._signals.scale_changed

    @property
    def percent(self) -> int:
        return self._percent

    @property
    def factor(self) -> float:
        return self._percent / 100.0

    def px(self, base) -> int:
        """把基准像素换算成当前比例下的整数像素（语义同 UIScaleManager.px）"""
        if not base:
            return 0
        value = round(base * self.factor)
        return max(1, int(value)) if base > 0 else min(-1, int(value))

    def pxf(self, base) -> float:
        """浮点版本，语义同 UIScaleManager.pxf：供描边宽度、圆角半径等需要
        亚像素精度、或要和 QPainter.scale() 配合使用的场景。"""
        return float(base) * self.factor

    def set_percent(self, percent) -> bool:
        """设置并持久化比例，发生变化时发出 scale_changed。返回是否真的变了。"""
        percent = UIScaleManager.normalize_percent(percent)
        changed = percent != self._percent
        self._percent = percent
        if self._config_manager:
            self._config_manager.set_app_setting("dialog_scale_percent", percent)
        if changed:
            self._signals.scale_changed.emit()
        return changed


def get_dialog_scale() -> DialogScaleManager:
    """获取独立窗口缩放管理器单例"""
    return DialogScaleManager.instance()


def dialog_scaled(base) -> int:
    """基准像素 → 当前独立窗口比例下的整数像素"""
    return DialogScaleManager.instance().px(base)


def dialog_scaled_f(base) -> float:
    """基准像素 → 当前独立窗口比例下的浮点像素（描边、圆角等）"""
    return DialogScaleManager.instance().pxf(base)


def widget_scaled(widget, base) -> int:
    """按控件上 configure_dialog_control() 标记的 dialog_scale_factor 属性换算像素。

    未被标记的控件（工具栏/面板里的 fluent_lite 控件）比例视为 1.0 —— 那部分
    走的是 UIScaleManager，两套缩放互不影响。供 ui/fluent_lite 的基础控件复用，
    避免每个文件各自重复实现同一段换算逻辑。
    """
    try:
        factor = float(widget.property("dialog_scale_factor") or 1.0) if widget is not None else 1.0
    except (TypeError, ValueError):
        factor = 1.0
    if not base:
        return 0
    value = round(base * factor)
    return max(1, int(value)) if base > 0 else min(-1, int(value))


def scale_dialog_font(widget) -> None:
    """将窗口的继承字体按当前独立窗口比例放大一次。"""
    factor = get_dialog_scale().factor
    if factor == 1.0:
        return

    font = widget.font()
    if font.pointSizeF() > 0:
        font.setPointSizeF(font.pointSizeF() * factor)
    elif font.pixelSize() > 0:
        font.setPixelSize(dialog_scaled(font.pixelSize()))
    widget.setFont(font)


def configure_dialog_control(widget) -> None:
    """Opt a Fluent control into standalone-window sizing before its layout is used.

    This deliberately touches one known control, rather than walking a completed
    widget tree and multiplying its current geometry.  Fluent controls use the
    marker when regenerating their own base stylesheet.
    """
    widget.setProperty("dialog_scale_factor", get_dialog_scale().factor)
    apply_theme = getattr(widget, "_apply_theme", None)
    if callable(apply_theme):
        apply_theme()


def configure_dialog_controls(root: QWidget) -> None:
    """对 root 子树下所有已接入 dialog_scale_factor 机制的控件批量打标记。

    新增一个设置分页或弹层时，不用为其中每个 fluent_lite 控件都手写一遍
    configure_dialog_control()：这里按 _apply_theme 是否存在筛选（那是控件
    自己从基准常量重新算样式的入口），一次性打完标记。跟 configure_dialog_control
    一样只触发控件"从基准值重算"，不读取也不搬用控件当前尺寸，所以同样不是
    test_dialog_scale.py 里禁止的"事后整体拉伸"。
    """
    configure_dialog_control(root)
    for widget in root.findChildren(QWidget):
        if callable(getattr(widget, "_apply_theme", None)):
            configure_dialog_control(widget)
