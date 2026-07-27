# -*- coding: utf-8 -*-
"""第 1 页 — 产品欢迎与界面语言。"""

from PySide6.QtWidgets import QVBoxLayout, QHBoxLayout, QLabel, QWidget
from PySide6.QtCore import Qt, QRectF
from PySide6.QtGui import QColor, QPainter, QPen, QIcon, QFont
from ui.fluent_lite import ComboBox

from core.i18n import make_tr
from core.logger import log_exception
from core import safe_event

if __package__:
    from .base_page import (
        BasePage, IllustrationArea, ACCENT, TEXT_PRIMARY, TEXT_SECOND,
        BG_ILLUS, PRODUCT_NAME, brand_text, welcome_theme,
        set_welcome_label_style, apply_welcome_label_style,
    )
else:
    import sys, os
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from base_page import (
        BasePage, IllustrationArea, ACCENT, TEXT_PRIMARY, TEXT_SECOND,
        BG_ILLUS, PRODUCT_NAME, brand_text, welcome_theme,
        set_welcome_label_style, apply_welcome_label_style,
    )


_tr = make_tr("WelcomeWizard")
_appearance_tr = make_tr("SettingsDialog")
_theme_label_tr = make_tr("ClipboardWindow")


# ── 首屏产品预览 ────────────────────────────────────────
class _ProductCanvas(QWidget):
    """展示 Jietuba 的四项核心能力，而非单一截图编辑器。"""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setMinimumHeight(220)
        self.setStyleSheet("background: transparent;")

    @safe_event
    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        w, h = self.width(), self.height()
        theme = welcome_theme()

        # 克制的背景装饰，给产品总览增加层次。
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(QColor(111, 143, 171, 34 if theme.is_dark else 24))
        p.drawEllipse(int(w * 0.60), 2, int(w * 0.40), int(w * 0.40))
        p.setBrush(QColor(96, 168, 151, 28 if theme.is_dark else 18))
        p.drawEllipse(-18, int(h * 0.62), int(w * 0.42), int(w * 0.42))

        card = QRectF(12, 18, max(80, w - 24), max(120, h - 36))
        p.setPen(QPen(QColor(theme.border), 1))
        p.setBrush(QColor(theme.panel))
        p.drawRoundedRect(card, 12, 12)

        # 产品工作台标题栏。
        top_h = 34
        p.setBrush(QColor(theme.panel_subtle))
        p.setPen(Qt.PenStyle.NoPen)
        p.drawRoundedRect(QRectF(card.x() + 1, card.y() + 1, card.width() - 2, top_h), 11, 11)
        p.fillRect(
            QRectF(card.x() + 1, card.y() + top_h - 10, card.width() - 2, 10),
            QColor(theme.panel_subtle),
        )
        for i, color in enumerate(
            (theme.text_soft, theme.border_strong, theme.border)
        ):
            p.setBrush(QColor(color))
            p.drawEllipse(int(card.x() + 13 + i * 12), int(card.y() + 13), 5, 5)

        content = card.adjusted(12, top_h + 10, -12, -12)

        # 顶部价值摘要。
        summary = QRectF(content.x(), content.y(), content.width(), 62)
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(QColor(theme.accent_soft))
        p.drawRoundedRect(summary, 9, 9)
        p.setPen(QColor(theme.accent))
        p.setFont(QFont("Segoe UI", 8, QFont.Weight.DemiBold))
        p.drawText(
            summary.adjusted(12, 8, -10, -34),
            Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter,
            "ONE WORKSPACE",
        )
        p.setPen(QColor(theme.text))
        p.setFont(QFont("Segoe UI", 10, QFont.Weight.DemiBold))
        p.drawText(
            summary.adjusted(12, 25, -8, -7),
            Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter,
            "Capture. Organize. Translate.",
        )

        # 四个产品能力模块。
        grid_y = summary.bottom() + 9
        gap = 8
        tile_w = (content.width() - gap) / 2
        available_h = max(120, content.bottom() - grid_y)
        tile_h = (available_h - gap) / 2
        modules = (
            ("Capture", "SCREEN", "#6F8FAB"),
            ("Annotate", "CREATE", "#6FA18F"),
            ("Clipboard", "ORGANIZE", "#8A7FA7"),
            ("Translate", "LANGUAGE", "#B18468"),
        )

        for idx, (title, meta, color) in enumerate(modules):
            col, row = idx % 2, idx // 2
            tile = QRectF(
                content.x() + col * (tile_w + gap),
                grid_y + row * (tile_h + gap),
                tile_w,
                tile_h,
            )
            p.setPen(QPen(QColor(theme.border), 1))
            p.setBrush(QColor(theme.panel))
            p.drawRoundedRect(tile, 9, 9)

            icon = QRectF(tile.x() + 10, tile.y() + 10, 27, 27)
            p.setPen(Qt.PenStyle.NoPen)
            icon_color = QColor(color)
            icon_color.setAlpha(32)
            p.setBrush(icon_color)
            p.drawRoundedRect(icon, 7, 7)
            self._draw_module_icon(p, idx, icon, QColor(color))

            p.setPen(QColor(theme.text))
            p.setFont(QFont("Segoe UI", 8, QFont.Weight.DemiBold))
            p.drawText(
                tile.adjusted(10, 41, -6, -18),
                Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter,
                title,
            )
            p.setPen(QColor(theme.text_soft))
            p.setFont(QFont("Segoe UI", 6, QFont.Weight.Medium))
            p.drawText(
                tile.adjusted(10, 57, -6, -5),
                Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter,
                meta,
            )

    def _apply_welcome_child_theme(self, _tokens=None):
        self.update()

    @staticmethod
    def _draw_module_icon(p: QPainter, index: int, rect: QRectF, color: QColor):
        p.setPen(QPen(color, 1.6))
        p.setBrush(Qt.BrushStyle.NoBrush)
        cx, cy = rect.center().x(), rect.center().y()
        if index == 0:
            p.drawRoundedRect(rect.adjusted(7, 8, -7, -8), 2, 2)
            p.drawEllipse(int(cx - 2), int(cy - 2), 4, 4)
        elif index == 1:
            p.drawLine(int(rect.x() + 8), int(rect.bottom() - 8),
                       int(rect.right() - 7), int(rect.y() + 7))
            p.drawLine(int(rect.x() + 8), int(rect.bottom() - 8),
                       int(rect.x() + 13), int(rect.bottom() - 9))
        elif index == 2:
            body = rect.adjusted(8, 7, -8, -6)
            p.drawRoundedRect(body, 2, 2)
            p.drawLine(int(cx - 4), int(rect.y() + 7), int(cx + 4), int(rect.y() + 7))
        else:
            p.setFont(QFont("Segoe UI", 9, QFont.Weight.Bold))
            p.drawText(rect, Qt.AlignmentFlag.AlignCenter, "A")


class _WelcomeIllus(IllustrationArea):
    def _build_content(self):
        # 品牌锁定区域
        brand = QHBoxLayout()
        brand.setSpacing(10)
        self._icon_lbl = QLabel(self)
        self._icon_lbl.setFixedSize(42, 42)
        self._icon_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self._try_load_icon()

        text_box = QVBoxLayout()
        text_box.setSpacing(0)
        self._name_lbl = QLabel(PRODUCT_NAME, self)
        set_welcome_label_style(
            self._name_lbl, role="primary", font_size=18, weight=700
        )
        self._meta_lbl = QLabel("PRODUCTIVITY SUITE", self)
        set_welcome_label_style(
            self._meta_lbl,
            role="accent",
            font_size=9,
            weight=600,
            extra="letter-spacing: 1px;",
        )
        text_box.addWidget(self._name_lbl)
        text_box.addWidget(self._meta_lbl)
        brand.addWidget(self._icon_lbl)
        brand.addLayout(text_box, 1)
        self._layout.addLayout(brand)

        self._preview = _ProductCanvas(self)
        self._layout.addWidget(self._preview, 1)

    def retranslate(self):
        self.update()

    def _apply_welcome_child_theme(self, _tokens=None):
        theme = welcome_theme()
        fallback_style = (
            f" font-size: 16px; font-weight: 700; color: {theme.accent};"
            if self._icon_lbl.property("welcomeFallbackIcon")
            else ""
        )
        self._icon_lbl.setStyleSheet(
            f"background: {theme.panel}; border: 1px solid {theme.border};"
            f" border-radius: 10px;{fallback_style}"
        )
        apply_welcome_label_style(self._name_lbl)
        apply_welcome_label_style(self._meta_lbl)
        self._preview.update()

    def _try_load_icon(self):
        try:
            from core.resource_manager import ResourceManager
            import os
            path = ResourceManager.get_resource_path("svg/托盘.svg")
            if os.path.exists(path):
                px = QIcon(path).pixmap(25, 25)
                self._icon_lbl.setPixmap(px)
                return
        except Exception as e:
            log_exception(e, "加载托盘图标")
        self._icon_lbl.setText("J")
        self._icon_lbl.setProperty("welcomeFallbackIcon", True)


# ── 页面主体 ────────────────────────────────────────────
class WelcomePage(BasePage):
    """第1页：欢迎 + 语言选择"""

    def __init__(self, config_manager, parent=None):
        self._config = config_manager

        # 读取当前已加载的语言（由 WelcomeWizard._init_language 提前设置好）
        try:
            from core.i18n import I18nManager
            self._init_lang = I18nManager.get_current_language()
        except Exception:
            self._init_lang = "ja"

        super().__init__(
            title=brand_text(_tr("欢迎使用截图吧 👋")).replace("👋", "").strip(),
            subtitle=_tr("截图 · 标注 · 剪贴板 · 翻译，一站搞定"),
            parent=parent,
        )

    # ── 插画 ────────────────────────────────────────────
    def _create_illustration(self):
        return _WelcomeIllus(self)

    # ── 控件 ────────────────────────────────────────────
    def _build_controls(self, layout: QVBoxLayout):
        # 语言
        self._row_lang_lbl = QLabel(_tr("🌐 界面语言").replace("🌐", "").strip())
        set_welcome_label_style(
            self._row_lang_lbl, role="primary", font_size=14, weight=600
        )
        layout.addWidget(self._row_lang_lbl)

        # 下拉框（靠左，固定宽度）
        self._lang_combo = ComboBox()
        self._lang_combo.setFixedWidth(200)
        self._lang_combo.setFixedHeight(36)
        self._lang_combo.setCursor(Qt.CursorShape.PointingHandCursor)

        try:
            from core.i18n import I18nManager
            current = getattr(self, "_init_lang", I18nManager.get_current_language())
            for code, name in I18nManager.get_available_languages().items():
                self._lang_combo.addItem(name, code)
            idx = self._lang_combo.findData(current)
            if idx >= 0:
                self._lang_combo.setCurrentIndex(idx)
        except Exception:
            self._lang_combo.addItem("日本語", "ja")
            self._lang_combo.addItem("English", "en")
            self._lang_combo.addItem("简体中文", "zh")

        self._lang_combo.currentIndexChanged.connect(self._on_lang_changed)

        # 用 HBoxLayout 让下拉框靠左（addStretch 推走右边空白）
        row = QHBoxLayout()
        row.setContentsMargins(0, 0, 0, 0)
        row.addWidget(self._lang_combo)
        row.addStretch()
        layout.addLayout(row)

        # 全局主题
        layout.addSpacing(20)
        self._theme_lbl = QLabel(_theme_label_tr("Theme"))
        set_welcome_label_style(
            self._theme_lbl, role="primary", font_size=14, weight=600
        )
        layout.addWidget(self._theme_lbl)

        self._theme_combo = ComboBox()
        self._theme_combo.setFixedWidth(200)
        self._theme_combo.setFixedHeight(36)
        self._theme_combo.setCursor(Qt.CursorShape.PointingHandCursor)
        self._populate_theme_combo()
        self._theme_combo.currentIndexChanged.connect(self._on_theme_changed)

        theme_row = QHBoxLayout()
        theme_row.setContentsMargins(0, 0, 0, 0)
        theme_row.addWidget(self._theme_combo)
        theme_row.addStretch()
        layout.addLayout(theme_row)

    def retranslate(self):
        """语言切换后由 wizard.retranslate_ui() 调用，刷新本页所有可见文字。"""
        self.title_label.setText(
            brand_text(_tr("欢迎使用截图吧 👋")).replace("👋", "").strip()
        )
        self.subtitle_label.setText(_tr("截图 · 标注 · 剪贴板 · 翻译，一站搞定"))
        if hasattr(self, "_row_lang_lbl") and self._row_lang_lbl:
            self._row_lang_lbl.setText(_tr("🌐 界面语言").replace("🌐", "").strip())
        if hasattr(self, "_theme_lbl") and self._theme_lbl:
            self._theme_lbl.setText(_theme_label_tr("Theme"))
        if hasattr(self, "_theme_combo") and self._theme_combo:
            self._populate_theme_combo()
        # 级联刷新插画区（打字机文字）
        if hasattr(self.illus_area, "retranslate"):
            self.illus_area.retranslate()

    # ── 逻辑 ────────────────────────────────────────────
    def _on_lang_changed(self):
        code = self._lang_combo.currentData()
        if not code:
            return
        try:
            from core.i18n import I18nManager
            I18nManager.load_language(code)
        except Exception as e:
            log_exception(e, "加载语言")
        # 保存到 config
        self._config.set_app_setting("language", code)
        # 通知父 wizard 刷新所有页面标题（如果父是 WelcomeWizard）
        wizard = self._find_wizard()
        if wizard is not None:
            wizard.retranslate_ui()

    def _populate_theme_combo(self):
        """使用现有全局主题值填充选项，并在语言变化时原位刷新文字。"""
        from core.ui_theme import get_ui_theme

        current = self._theme_combo.currentData() if self._theme_combo.count() else None
        current = current or get_ui_theme().mode.value

        self._theme_combo.blockSignals(True)
        self._theme_combo.clear()
        self._theme_combo.addItem(_appearance_tr("System"), "system")
        self._theme_combo.addItem(_appearance_tr("Light"), "light")
        self._theme_combo.addItem(_appearance_tr("Dark"), "dark")
        index = self._theme_combo.findData(current)
        self._theme_combo.setCurrentIndex(max(0, index))
        self._theme_combo.blockSignals(False)

    def _on_theme_changed(self, _index=None):
        """将选择直接交给现有全局主题管理器。"""
        mode = self._theme_combo.currentData()
        if not mode:
            return
        try:
            from core.ui_theme import get_ui_theme

            # 欢迎页持有真实配置实例；主题管理器只负责立即应用和发出信号。
            self._config.set_app_setting("ui_theme_mode", mode)
            get_ui_theme().set_mode(mode, persist=False)
        except Exception as e:
            log_exception(e, "切换界面主题")

    def _find_wizard(self):
        """向上找到 WelcomeWizard 实例（在 QStackedWidget 里）"""
        p = self.parent()
        while p is not None:
            # 避免循环导入，用类名字符串判断
            if type(p).__name__ == "WelcomeWizard":
                return p
            p = p.parent()
        return None

    def save(self):
        """向导结束时调用，持久化设置"""
        pass  # 已在 _on_lang_changed 实时保存


if __name__ == "__main__":
    import sys, os
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from base_page import _dev_bootstrap
    mock = _dev_bootstrap()

    from PySide6.QtWidgets import QApplication
    from wizard import WelcomeWizard

    app = QApplication(sys.argv)
    w = WelcomeWizard(mock)
    w._stack.setCurrentIndex(0)   # 跳到第1页
    w._update_nav()
    w.show()
    sys.exit(app.exec())
 
