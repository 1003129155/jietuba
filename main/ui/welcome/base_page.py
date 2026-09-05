# -*- coding: utf-8 -*-
"""欢迎向导的共享页面骨架与视觉规范。"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Optional, Tuple

from PySide6.QtCore import Qt, QSize, Signal
from PySide6.QtGui import QPixmap
from PySide6.QtWidgets import QWidget, QVBoxLayout, QHBoxLayout, QLabel, QFrame, QSizePolicy
from ui.fluent_lite import SwitchButton
# ACCENT 在本文件内未直接使用，但 page4_smart_select 是从这里导入它的，
# 属于有意的转发导出，不要删。
from ui.fluent_lite.theme import ACCENT, ACCENT_HOVER  # noqa: F401
from core.ui_theme import get_ui_theme


# ─────────────────────────────────────────
# 设计规范（欢迎向导全局共享）
# ─────────────────────────────────────────
ACCENT_DARK = ACCENT_HOVER  # 兼容欢迎页现有命名
TEXT_PRIMARY = "#172331"
TEXT_SECOND = "#667587"
BG_PAGE = "#FFFFFF"
BG_ILLUS = "#F3F7FA"
BG_SUBTLE = "#F7F9FB"
BORDER = "#DDE5EC"
RADIUS = 16
PRODUCT_NAME = "Jietuba"


@dataclass(frozen=True)
class WelcomeTheme:
    """欢迎向导使用的语义颜色，跟随全局明暗主题。"""

    page: str
    panel: str
    panel_subtle: str
    illustration: str
    sidebar: str
    border: str
    border_strong: str
    separator: str
    text: str
    text_muted: str
    text_soft: str
    accent: str
    accent_hover: str
    accent_soft: str
    toolbar: str
    canvas: str
    is_dark: bool


def welcome_theme() -> WelcomeTheme:
    """从全局主题管理器解析欢迎向导语义颜色。"""
    tokens = get_ui_theme().tokens
    if tokens.is_dark:
        return WelcomeTheme(
            page="#23272C",
            panel="#2B3036",
            panel_subtle="#252A30",
            illustration="#262C32",
            sidebar="#20252A",
            border="#3C444D",
            border_strong="#4B5661",
            separator="#363E47",
            text="#F2F4F6",
            text_muted="#AEB7C1",
            text_soft="#87919C",
            accent=tokens.accent,
            accent_hover=tokens.accent_hover,
            accent_soft="#31404C",
            toolbar="#171C21",
            canvas="#303840",
            is_dark=True,
        )
    return WelcomeTheme(
        page="#FFFFFF",
        panel="#FFFFFF",
        panel_subtle="#F7F9FB",
        illustration="#F3F7FA",
        sidebar="#F5F8FA",
        border="#DDE5EC",
        border_strong="#C9D6E0",
        separator="#E5EBF0",
        text="#172331",
        text_muted="#667587",
        text_soft="#8795A4",
        accent=tokens.accent,
        accent_hover=tokens.accent_hover,
        accent_soft="#E9F0F5",
        toolbar="#233342",
        canvas="#EAF0F4",
        is_dark=False,
    )


def brand_text(text: str) -> str:
    """将翻译文件中的历史产品名统一成当前英文品牌写法。"""
    result = str(text)
    for legacy_name in ("截图吧", "JieTuBa", "JieTuba", "JIETUBA"):
        result = result.replace(legacy_name, PRODUCT_NAME)
    return result


def set_welcome_label_style(
    label: QLabel,
    *,
    role: str = "primary",
    font_size: int = 13,
    weight: int = 400,
    extra: str = "",
) -> QLabel:
    """标记标签的语义样式，由 BasePage 在主题变化时统一刷新。"""
    label.setProperty("welcomeTextRole", role)
    label.setProperty("welcomeFontSize", font_size)
    label.setProperty("welcomeFontWeight", weight)
    label.setProperty("welcomeStyleExtra", extra)
    apply_welcome_label_style(label)
    return label


def apply_welcome_label_style(label: QLabel) -> None:
    role = label.property("welcomeTextRole")
    if not role:
        return
    theme = welcome_theme()
    color = {
        "primary": theme.text,
        "muted": theme.text_muted,
        "soft": theme.text_soft,
        "accent": theme.accent,
    }.get(str(role), theme.text)
    size = int(label.property("welcomeFontSize") or 13)
    weight = int(label.property("welcomeFontWeight") or 400)
    extra = str(label.property("welcomeStyleExtra") or "")
    label.setStyleSheet(
        f"font-size: {size}px; font-weight: {weight}; color: {color};"
        f" background: transparent; border: none; {extra}"
    )


# ─────────────────────────────────────────
# ToggleSwitch（兼容旧调用名，实际复用 fluent_lite 的统一控件）
# ─────────────────────────────────────────
class ToggleSwitch(SwitchButton):
    toggled = Signal(bool)

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setFixedSize(44, 24)
        self.checkedChanged.connect(self.toggled.emit)


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
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)

        self._layout = QVBoxLayout(self)
        self._layout.setContentsMargins(22, 22, 22, 22)
        self._layout.setSpacing(12)
        self._layout.setAlignment(Qt.AlignmentFlag.AlignCenter)

        self._build_content()
        self._apply_welcome_theme()

    def _apply_welcome_theme(self, _tokens=None):
        theme = welcome_theme()
        self.setStyleSheet(f"""
            #IllustrationArea {{
                background: {theme.illustration};
                border: 1px solid {theme.border};
                border-radius: {RADIUS}px;
            }}
        """)
        self.update()

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

    页面顶部负责解释当前步骤，主体将功能预览与设置面板并排展示。
    所有子页继续只需提供 illustration 和 controls。
    """

    def __init__(self, title: str = "", subtitle: str = "", parent=None):
        super().__init__(parent)
        self.setObjectName("BasePage")

        root = QVBoxLayout(self)
        root.setContentsMargins(30, 24, 30, 22)
        root.setSpacing(20)

        # —— 顶部：本步骤说明 ——
        header = QWidget(self)
        header.setStyleSheet("background: transparent;")
        header_layout = QVBoxLayout(header)
        header_layout.setContentsMargins(0, 0, 0, 0)
        header_layout.setSpacing(7)

        self.title_label: Optional[QLabel] = None
        if title:
            self.title_label = QLabel(title, header)
            self.title_label.setWordWrap(True)
            set_welcome_label_style(
                self.title_label, role="primary", font_size=27, weight=700
            )
            header_layout.addWidget(self.title_label)

        self.subtitle_label: Optional[QLabel] = None
        if subtitle:
            self.subtitle_label = QLabel(subtitle, header)
            self.subtitle_label.setWordWrap(True)
            set_welcome_label_style(
                self.subtitle_label, role="muted", font_size=13, weight=400
            )
            header_layout.addWidget(self.subtitle_label)
        root.addWidget(header)

        # —— 主体：左侧功能预览，右侧配置卡片 ——
        body = QHBoxLayout()
        body.setContentsMargins(0, 0, 0, 0)
        body.setSpacing(18)

        self.illus_area = self._create_illustration()
        self.illus_area.setFixedWidth(300)
        body.addWidget(self.illus_area)

        content_widget = QFrame(self)
        content_widget.setObjectName("ContentPanel")
        content_widget.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)
        self._content_widget = content_widget

        self.content_layout = QVBoxLayout(content_widget)
        self.content_layout.setContentsMargins(24, 24, 24, 20)
        self.content_layout.setSpacing(10)

        self._build_controls(self.content_layout)
        self.content_layout.addStretch()
        body.addWidget(content_widget, 1)
        root.addLayout(body, 1)

        self._theme_manager = get_ui_theme()
        self._theme_manager.theme_changed.connect(self._apply_welcome_theme)
        self._apply_welcome_theme(self._theme_manager.tokens)

    def _apply_welcome_theme(self, tokens=None):
        theme = welcome_theme()
        self.setStyleSheet(f"#BasePage {{ background: {theme.page}; }}")
        self._content_widget.setStyleSheet(f"""
            #ContentPanel {{
                background: {theme.panel};
                border: 1px solid {theme.border};
                border-radius: {RADIUS}px;
            }}
        """)
        self.illus_area._apply_welcome_theme(tokens)

        for label in self.findChildren(QLabel):
            apply_welcome_label_style(label)
        for widget in self.findChildren(QWidget):
            if widget.property("welcomeSettingRow"):
                widget.setStyleSheet(f"""
                    #SettingRow {{
                        background: {theme.panel_subtle};
                        border: 1px solid {theme.border};
                        border-radius: 10px;
                    }}
                """)
            callback = getattr(widget, "_apply_welcome_child_theme", None)
            if callable(callback):
                callback(tokens)
        self.update()

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
        container.setObjectName("SettingRow")
        container.setProperty("welcomeSettingRow", True)

        vbox = QVBoxLayout(container)
        vbox.setContentsMargins(14, 11, 12, 11)
        vbox.setSpacing(4)

        row = QHBoxLayout()
        row.setSpacing(12)

        lbl = QLabel(label_text, container)
        set_welcome_label_style(lbl, role="primary", font_size=14, weight=600)
        row.addWidget(lbl)
        row.addStretch()
        row.addWidget(widget)

        vbox.addLayout(row)

        desc_lbl: Optional[QLabel] = None
        if description:
            desc_lbl = QLabel(description, container)
            desc_lbl.setWordWrap(True)
            set_welcome_label_style(desc_lbl, role="muted", font_size=12, weight=400)
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
        def get_clipboard_history_limit(self):
            return self._def("clipboard_history_limit", 1000)
        def set_clipboard_history_limit(self, v): pass

        def get_translation_hotkey(self):
            return self._def("translation_hotkey", "")
        def get_translation_hotkey_2(self):
            return self._def("translation_hotkey_2", "")
        def set_translation_hotkey(self, v): pass
        def set_translation_hotkey_2(self, v): pass

        def get_clipboard_enabled(self): return self._def("clipboard_enabled", True)
        def set_clipboard_enabled(self, v): pass

        def get_smart_selection(self): return self._def("smart_selection", True)
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

        def get_translation_provider(self):
            return self._def("translation_provider", "google")
        def set_translation_provider(self, v): pass

        def get_google_translate_api_key(self):
            return self._def("google_translate_api_key", "")
        def set_google_translate_api_key(self, v): pass

        def get_amazon_translate_region(self):
            return self._def("amazon_translate_region", "us-west-2")
        def set_amazon_translate_region(self, v): pass
        def get_amazon_translate_access_key_id(self):
            return self._def("amazon_translate_access_key_id", "")
        def set_amazon_translate_access_key_id(self, v): pass
        def get_amazon_translate_secret_access_key(self):
            return self._def("amazon_translate_secret_access_key", "")
        def set_amazon_translate_secret_access_key(self, v): pass
        def get_amazon_translate_session_token(self):
            return self._def("amazon_translate_session_token", "")
        def set_amazon_translate_session_token(self, v): pass

        def get_app_setting(self, key, default=None):
            return self._d.get(key, default)

        def set_app_setting(self, key, value): pass
        def mark_as_run(self): pass
        def is_first_run(self): return True

    # 挂到模块级，调用方可以 from base_page import MockConfig
    sys.modules[__name__].__dict__["MockConfig"] = _MockConfig
    return _MockConfig() 
