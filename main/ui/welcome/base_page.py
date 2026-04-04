# -*- coding: utf-8 -*-
"""
欢迎向导 - 页面基类

提供统一的页面结构：
  - 上半部分：图片/动画展示区（IllustrationArea）
  - 下半部分：内容区（标题、说明文、控件）
  - 共享调色板和字体规范
"""

from __future__ import annotations

from typing import Optional, Tuple

from PySide6.QtCore import Qt, QSize, QPropertyAnimation, QEasingCurve, Property, Signal
from PySide6.QtGui import QColor, QPainter, QPixmap
from PySide6.QtWidgets import (
    QWidget,
    QVBoxLayout,
    QHBoxLayout,
    QLabel,
    QFrame,
    QSizePolicy,
)
from core import safe_event


# ─────────────────────────────────────────
# 设计规范（全局共享）
# ─────────────────────────────────────────
ACCENT = "#2196F3"        # 主色调（蓝）
ACCENT_DARK = "#1565C0"   # 深蓝（hover）
TEXT_PRIMARY = "#1A1A2E"  # 主文字
TEXT_SECOND = "#5C6370"   # 次要文字
BG_PAGE = "#FFFFFF"       # 页面背景
BG_ILLUS = "#F0F7FF"      # 插画区背景
RADIUS = 12               # 圆角半径

# 插画区布局策略（二选一）
# 窗口总高 720px，底部导航栏 62px，分隔线 1px → 页面可用高度 = 657px
# ILLUS_RATIO 控制插画区占页面可用高度的比例，修改这里即可。
_PAGE_H = 520 - 62 - 1        # 页面可用高度（与 WelcomeWizard.WINDOW_H / nav 保持同步）
ILLUS_RATIO = 0.78             # 插画区占页面可用高度比例（stretch，仅 USE_FIXED_ILLUS_HEIGHT=False 时生效）
ILLUS_FIXED_HEIGHT = int(_PAGE_H * ILLUS_RATIO)  # 固定像素高度，随 ILLUS_RATIO 自动计算
USE_FIXED_ILLUS_HEIGHT = True  # True: 固定高度；False: 使用比例 stretch


# ─────────────────────────────────────────
# ToggleSwitch（与 settings_window 同款，避免循环导入直接复制）
# ─────────────────────────────────────────
class ToggleSwitch(QWidget):
    toggled = Signal(bool)

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setFixedSize(44, 24)
        self.setCursor(Qt.CursorShape.PointingHandCursor)

        self._checked = False
        self._circle_pos = 3.0

        self._anim = QPropertyAnimation(self, b"circle_pos", self)
        self._anim.setDuration(160)
        self._anim.setEasingCurve(QEasingCurve.Type.OutCubic)

    # Qt property for animation
    def _get_cp(self) -> float:
        return float(self._circle_pos)

    def _set_cp(self, v: float) -> None:
        self._circle_pos = float(v)
        self.update()

    circle_pos = Property(float, _get_cp, _set_cp)

    def isChecked(self) -> bool:
        return bool(self._checked)

    def setChecked(self, checked: bool) -> None:
        checked = bool(checked)
        if self._checked == checked:
            return

        self._checked = checked
        target = (self.width() - 21.0) if checked else 3.0

        self._anim.stop()
        self._anim.setStartValue(self._circle_pos)
        self._anim.setEndValue(target)
        self._anim.start()

    @safe_event
    def mousePressEvent(self, event) -> None:
        if event.button() == Qt.MouseButton.LeftButton:
            self.setChecked(not self._checked)
            self.toggled.emit(self._checked)
        super().mousePressEvent(event)

    @safe_event
    def paintEvent(self, event) -> None:
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)

        r = self.rect()
        h = r.height()

        # 轨道
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(QColor(ACCENT if self._checked else "#E5E5E5"))
        p.drawRoundedRect(0, 0, r.width(), h, h / 2, h / 2)

        # 圆圈
        cx = int(self._circle_pos)
        p.setBrush(QColor("#FFFFFF"))
        p.drawEllipse(cx, 3, 18, 18)


# ─────────────────────────────────────────
# IllustrationArea — 上半部插画/动画区
# ─────────────────────────────────────────
class IllustrationArea(QFrame):
    """
    页面上半部分的插画展示区。
    子类可以重写 _build_content() 在区域内放置自定义内容。
    默认显示一个纯色背景 + 可选图片。
    """

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setObjectName("IllustrationArea")
        self.setStyleSheet(f"""
            #IllustrationArea {{
                background: {BG_ILLUS};
                border-radius: {RADIUS}px {RADIUS}px 0 0;
            }}
        """)
        # 固定高度模式下使用 Fixed 垂直策略，防止布局引擎覆盖 setFixedHeight；
        # 非固定高度模式保留 Expanding 以便 stretch 正常工作。
        if USE_FIXED_ILLUS_HEIGHT:
            self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        else:
            self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)

        self._layout = QVBoxLayout(self)
        self._layout.setContentsMargins(24, 24, 24, 24)
        self._layout.setAlignment(Qt.AlignmentFlag.AlignCenter)

        self._build_content()

    def _build_content(self) -> None:
        """子类重写，在插画区内添加内容"""
        # 默认不放任何内容
        return

    def set_pixmap(self, pixmap: QPixmap, max_size: QSize = QSize(280, 180)) -> None:
        """便捷方法：在区域中央显示一张图片"""
        lbl = QLabel(self)
        lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
        scaled = pixmap.scaled(
            max_size,
            Qt.AspectRatioMode.KeepAspectRatio,
            Qt.TransformationMode.SmoothTransformation,
        )
        lbl.setPixmap(scaled)
        self._layout.addWidget(lbl)


# ─────────────────────────────────────────
# BasePage — 所有欢迎页面的基类
# ─────────────────────────────────────────
class BasePage(QWidget):
    """
    欢迎向导页面基类。

    结构：
        ┌─────────────────────────────────┐
        │   IllustrationArea (上半部分)    │  ← 子类可替换
        ├─────────────────────────────────┤
        │   content_layout  (下半部分)     │
        │     title_label                 │
        │     subtitle_label              │
        │     ── 自定义控件区 ──          │  ← _build_controls() 钩子
        └─────────────────────────────────┘
    """

    def __init__(self, title: str = "", subtitle: str = "", parent=None):
        super().__init__(parent)
        self.setObjectName("BasePage")
        self.setStyleSheet(f"#BasePage {{ background: {BG_PAGE}; }}")

        root = QVBoxLayout(self)
        root.setContentsMargins(0, 0, 0, 0)
        root.setSpacing(0)

        # —— 上半部：插画区 ——
        self.illus_area = self._create_illustration()

        if USE_FIXED_ILLUS_HEIGHT:
            self.illus_area.setFixedHeight(ILLUS_FIXED_HEIGHT)
            root.addWidget(self.illus_area)
        else:
            root.addWidget(self.illus_area, int(ILLUS_RATIO * 100))

        # 分隔线
        sep = QFrame(self)
        sep.setFixedHeight(1)
        sep.setStyleSheet("background: #E8ECF0;")
        root.addWidget(sep)

        # —— 下半部：内容区 ——
        content_widget = QWidget(self)
        content_widget.setStyleSheet(f"background: {BG_PAGE};")

        self.content_layout = QVBoxLayout(content_widget)
        self.content_layout.setContentsMargins(36, 28, 36, 20)
        self.content_layout.setSpacing(10)

        # 标题
        self.title_label: Optional[QLabel] = None
        if title:
            self.title_label = QLabel(title, content_widget)
            self.title_label.setWordWrap(True)
            self.title_label.setStyleSheet(
                f"font-size: 20px; font-weight: 700; color: {TEXT_PRIMARY};"
                f" background: transparent;"
            )
            self.content_layout.addWidget(self.title_label)

        # 副标题/说明文
        self.subtitle_label: Optional[QLabel] = None
        if subtitle:
            self.subtitle_label = QLabel(subtitle, content_widget)
            self.subtitle_label.setWordWrap(True)
            self.subtitle_label.setStyleSheet(
                f"font-size: 13px; color: {TEXT_SECOND}; line-height: 1.6;"
                f" background: transparent;"
            )
            self.content_layout.addWidget(self.subtitle_label)

        # 自定义控件钩子
        self._build_controls(self.content_layout)

        self.content_layout.addStretch()

        if USE_FIXED_ILLUS_HEIGHT:
            root.addWidget(content_widget)
        else:
            root.addWidget(content_widget, 100 - int(ILLUS_RATIO * 100))

    # ── 子类钩子 ──────────────────────────────────

    def _create_illustration(self) -> IllustrationArea:
        """子类可返回自定义的 IllustrationArea 子类"""
        return IllustrationArea(self)

    def _build_controls(self, layout: QVBoxLayout) -> None:
        """子类在这里向 content_layout 添加控件"""
        return

    # ── 工具方法 ──────────────────────────────────

    @staticmethod
    def _make_setting_row(label_text: str, widget: QWidget, description: str = "") -> QWidget:
        """
        创建一行「左文字 + 右控件」的设置行，可选附加说明文字。
        返回一个容器 QWidget，直接 addWidget 到 layout 即可。
        """
        container, _, _ = BasePage._make_setting_row_with_refs(label_text, widget, description)
        return container

    @staticmethod
    def _make_setting_row_with_refs(
        label_text: str, widget: QWidget, description: str = ""
    ) -> Tuple[QWidget, QLabel, Optional[QLabel]]:
        """
        同 _make_setting_row，但额外返回 label 和 desc_label 的引用，
        方便 retranslate() 时更新文字。
        返回 (container, label_widget, desc_label_widget_or_None)
        """
        container = QWidget()
        container.setStyleSheet("background: transparent;")

        vbox = QVBoxLayout(container)
        vbox.setContentsMargins(0, 0, 0, 0)
        vbox.setSpacing(3)

        row = QHBoxLayout()
        row.setSpacing(12)

        lbl = QLabel(label_text, container)
        lbl.setStyleSheet(
            f"font-size: 14px; font-weight: 600; color: {TEXT_PRIMARY};"
            " background: transparent;"
        )
        row.addWidget(lbl)
        row.addStretch()
        row.addWidget(widget)

        vbox.addLayout(row)

        desc_lbl: Optional[QLabel] = None
        if description:
            desc_lbl = QLabel(description, container)
            desc_lbl.setWordWrap(True)
            desc_lbl.setStyleSheet(
                f"font-size: 12px; color: {TEXT_SECOND}; background: transparent;"
            )
            vbox.addWidget(desc_lbl)

        return container, lbl, desc_lbl

def _dev_bootstrap():
    """
    单文件直接运行时的环境引导。
    """
    import sys
    import os
    import importlib
    import types

    here = os.path.dirname(os.path.abspath(__file__))  # .../main/ui/welcome
    ui_dir = os.path.dirname(here)                     # .../main/ui
    main_dir = os.path.dirname(ui_dir)                 # .../main

    for p in (here, ui_dir, main_dir):
        if p not in sys.path:
            sys.path.insert(0, p)

    pkg_name = "ui.welcome"
    if pkg_name not in sys.modules:
        pkg = types.ModuleType(pkg_name)
        pkg.__path__ = [here]
        pkg.__package__ = pkg_name
        pkg.__spec__ = importlib.util.spec_from_file_location(
            pkg_name,
            os.path.join(here, "__init__.py"),
            submodule_search_locations=[here],
        )
        sys.modules[pkg_name] = pkg

    if "ui" not in sys.modules:
        ui_pkg = types.ModuleType("ui")
        ui_pkg.__path__ = [ui_dir]
        ui_pkg.__package__ = "ui"
        sys.modules["ui"] = ui_pkg

    class _MockConfig:
        """调试用 config，优先从真实 APP_DEFAULT_SETTINGS 读取默认值。"""

        def __init__(self):
            try:
                from settings.tool_settings import ToolSettingsManager
                _d = ToolSettingsManager.APP_DEFAULT_SETTINGS
            except Exception:
                _d = {}
            self._d = dict(_d) if isinstance(_d, dict) else {}

        def _def(self, key, fallback=None):
            return self._d.get(key, fallback)

        def get_hotkey(self): return self._def("hotkey", "ctrl+1")
        def get_hotkey_2(self): return self._def("hotkey_2", "")
        def set_hotkey(self, v): pass
        def set_hotkey_2(self, v): pass

        def get_screenshot_save_path(self):
            return self._def("screenshot_save_path", "")

        def set_screenshot_save_path(self, v):
            pass

        def get_clipboard_hotkey(self): return self._def("clipboard_hotkey", "ctrl+2")
        def get_clipboard_hotkey_2(self): return self._def("clipboard_hotkey_2", "")
        def set_clipboard_hotkey(self, v): pass
        def set_clipboard_hotkey_2(self, v): pass

        def get_clipboard_enabled(self): return self._def("clipboard_enabled", True)
        def set_clipboard_enabled(self, v): pass

        def get_smart_selection(self): return self._def("smart_selection", False)
        def set_smart_selection(self, v): pass

        def get_ocr_enabled(self): return self._def("ocr_enabled", True)
        def set_ocr_enabled(self, v): pass

        def get_clipboard_group_bar_position(self): return "right"
        def set_clipboard_group_bar_position(self, mode): pass

        def get_show_main_window(self): return False
        def get_autostart(self): return False

        def get_deepl_api_key(self): return self._def("deepl_api_key", "")
        def set_deepl_api_key(self, v): pass

        def get_deepl_use_pro(self): return self._def("deepl_use_pro", False)
        def set_deepl_use_pro(self, v): pass

        def get_app_setting(self, key, default=None):
            return self._d.get(key, default)

        def set_app_setting(self, key, value): pass
        def mark_as_run(self): pass
        def is_first_run(self): return True

    # 挂到模块级，调用方可以 from base_page import MockConfig
    sys.modules[__name__].__dict__["MockConfig"] = _MockConfig
    return _MockConfig() 