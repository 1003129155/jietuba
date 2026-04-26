# -*- coding: utf-8 -*-
"""
第5页 — 翻译设置

上半部：文字翻译流向动画
下半部：DeepL API Key 输入 + 目标语言选择
"""

from PySide6.QtWidgets import (
    QVBoxLayout, QLabel, QLineEdit, QWidget, QHBoxLayout
)
from PySide6.QtCore import Qt, QTimer
from PySide6.QtGui import QPainter, QColor, QFont, QPen, QPainterPath
from qfluentwidgets import ComboBox, LineEdit
from core import safe_event
from core.i18n import make_tr

if __package__:
    from .base_page import BasePage, IllustrationArea, ACCENT, TEXT_PRIMARY, TEXT_SECOND
else:
    import sys, os
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from base_page import BasePage, IllustrationArea, ACCENT, TEXT_PRIMARY, TEXT_SECOND


_tr = make_tr("WelcomeWizard")


# ── 插画区：文字翻译流动动画 ────────────────────────────
class _TranslateIllus(IllustrationArea):
    def _build_content(self):
        from PySide6.QtWidgets import QSizePolicy
        self._canvas = _TransAnim(self)
        self._canvas.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)
        self._layout.addWidget(self._canvas)

    def retranslate(self):
        if hasattr(self, "_canvas"):
            self._canvas.retranslate()


# 每种界面语言对应的演示文字
# key = I18nManager language code
# value = (src_text, src_lang_label, dst_text, dst_lang_label)
_DEMO_MAP = {
    "zh": ("你好，世界！",  "中文",    "Hello, World!", "English"),
    "en": ("Hello, World!", "English", "你好，世界！",   "中文"),
    "ja": ("こんにちは！",  "日本語",  "Hello, World!", "English"),
}
_DEMO_DEFAULT = ("Hello, World!", "English", "你好，世界！", "中文")


class _TransAnim(QWidget):
    # 动画阶段：
    # 0 = 显示源文字（静止）
    # 1 = 源文字淡出
    # 2 = 目标文字淡入
    # 3 = 显示目标文字（静止）

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setStyleSheet("background: transparent;")
        self._phase = 0
        self._alpha = 1.0          # 当前渐变进度 0.0-1.0
        self._refresh_texts()

        self._timer = QTimer(self)
        self._timer.timeout.connect(self._tick)
        self._schedule_next(1400)  # 先静止 1.4s 再开始淡出

    # ── 语言刷新 ──────────────────────────────────────
    def _refresh_texts(self):
        try:
            from core.i18n import I18nManager
            lang = I18nManager.get_current_language()
        except Exception as e:
            from core.logger import log_exception
            log_exception(e, "获取当前语言")
            lang = "zh"
        row = _DEMO_MAP.get(lang, _DEMO_DEFAULT)
        self._src_text, self._src_lang, self._dst_text, self._dst_lang = row

    def retranslate(self):
        self._refresh_texts()
        self.update()

    # ── 动画驱动 ──────────────────────────────────────
    def _schedule_next(self, ms):
        QTimer.singleShot(ms, self._advance_phase)

    def _advance_phase(self):
        self._phase = (self._phase + 1) % 4
        self._alpha = 1.0 if self._phase in (0, 3) else 0.0
        if self._phase in (0, 3):          # 静止阶段
            self._timer.stop()
            self._schedule_next(1400)
        else:                               # 渐变阶段
            self._timer.start(16)

    def _tick(self):
        self._alpha = min(1.0, self._alpha + 0.045)
        if self._alpha >= 1.0:
            self._alpha = 1.0
            self._timer.stop()
            self._schedule_next(80)
        self.update()

    # ── 绘制 ──────────────────────────────────────────
    @safe_event
    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        w, h = self.width(), self.height()

        card_w = min(w - 40, 280)
        card_h = 58
        cx = (w - card_w) // 2
        gap = 14
        cy_src = h // 2 - card_h - gap // 2
        cy_dst = h // 2 + gap // 2

        # ── 卡片背景 ──
        p.setPen(QPen(QColor("#D0DEF0"), 1))
        p.setBrush(QColor(255, 255, 255, 210))
        p.drawRoundedRect(cx, cy_src, card_w, card_h, 8, 8)
        p.drawRoundedRect(cx, cy_dst, card_w, card_h, 8, 8)

        # ── 语言标签 ──
        tag_font = QFont("Microsoft YaHei", 8)
        p.setFont(tag_font)
        p.setPen(QColor(ACCENT))
        p.drawText(cx + 10, cy_src + 4, card_w - 20, 16,
                   Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter,
                   self._src_lang)
        p.drawText(cx + 10, cy_dst + 4, card_w - 20, 16,
                   Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter,
                   self._dst_lang)

        # ── 文字 alpha 计算 ──
        # phase 0: src=255, dst=0
        # phase 1: src 从 255→0  (alpha 0→1)
        # phase 2: dst 从 0→255  (alpha 0→1)
        # phase 3: src=0,  dst=255
        if self._phase == 0:
            src_a, dst_a = 255, 0
        elif self._phase == 1:
            src_a = int(255 * (1.0 - self._alpha))
            dst_a = 0
        elif self._phase == 2:
            src_a = 0
            dst_a = int(255 * self._alpha)
        else:   # phase 3
            src_a, dst_a = 0, 255

        text_font = QFont("Microsoft YaHei", 13, QFont.Weight.Medium)
        p.setFont(text_font)

        if src_a > 0:
            p.setPen(QColor(26, 26, 46, src_a))
            p.drawText(cx + 10, cy_src + 20, card_w - 20, card_h - 24,
                       Qt.AlignmentFlag.AlignVCenter, self._src_text)

        if dst_a > 0:
            p.setPen(QColor(26, 26, 46, dst_a))
            p.drawText(cx + 10, cy_dst + 20, card_w - 20, card_h - 24,
                       Qt.AlignmentFlag.AlignVCenter, self._dst_text)

        # ── 箭头（翻译进行中时高亮）──
        arrow_a = 200 if self._phase in (1, 2, 3) else 80
        pen = QPen(QColor(33, 150, 243, arrow_a), 2)
        pen.setCapStyle(Qt.PenCapStyle.RoundCap)
        p.setPen(pen)
        ax = w // 2
        ay1 = cy_src + card_h + 3
        ay2 = cy_dst - 3
        p.drawLine(ax, ay1, ax, ay2 - 6)
        # 箭头头部
        p.drawLine(ax - 5, ay2 - 7, ax, ay2)
        p.drawLine(ax + 5, ay2 - 7, ax, ay2)


# ── 页面主体 ────────────────────────────────────────────
class TranslationPage(BasePage):
    """第5页：翻译 API 设置"""

    def __init__(self, config_manager, parent=None):
        self._config = config_manager
        super().__init__(
            title="🌐 翻译设置",
            subtitle="使用 DeepL API 进行高质量翻译。\n"
                     "免费账户每月 50 万字符额度，够日常使用。",
            parent=parent,
        )

    def _create_illustration(self):
        return _TranslateIllus(self)

    def _build_controls(self, layout: QVBoxLayout):
        # API Key 输入
        self._api_edit = LineEdit()
        self._api_edit.setPlaceholderText("留空则使用免费公共接口")
        self._api_edit.setFixedHeight(32)
        self._api_edit.setEchoMode(QLineEdit.EchoMode.Password)
        existing = ""
        if hasattr(self._config, "get_deepl_api_key"):
            existing = self._config.get_deepl_api_key() or ""
        self._api_edit.setText(existing)

        # 不用 _make_setting_row（需要输入框拉满宽度）
        self._api_lbl = QLabel("DeepL API Key")
        self._api_lbl.setStyleSheet(
            f"font-size: 14px; font-weight: 600; color: {TEXT_PRIMARY};"
            " background: transparent;"
        )
        layout.addWidget(self._api_lbl)
        layout.addWidget(self._api_edit)

        self._hint_lbl = QLabel(
            '：<a href="https://www.deepl.com/pro-api" '
            'style="color:#2196F3;">deepl.com/pro-api</a>'
        )
        self._hint_lbl.setOpenExternalLinks(True)
        self._hint_lbl.setStyleSheet(f"font-size: 12px; color: {TEXT_SECOND}; background: transparent;")
        layout.addWidget(self._hint_lbl)

        # 目标语言
        self._lang_combo = ComboBox()
        self._lang_combo.setFixedWidth(160)
        self._lang_combo.setFixedHeight(32)
        self._lang_combo.setCursor(Qt.CursorShape.PointingHandCursor)
        self._populate_lang_combo()
        row, self._row_lang_lbl, _ = self._make_setting_row_with_refs(
            "翻译目标语言",
            self._lang_combo,
        )
        layout.addWidget(row)

    def retranslate(self):
        self.title_label.setText(_tr("🌐 翻译设置"))
        self.subtitle_label.setText(_tr(
            "使用 DeepL API 进行高质量翻译。\n"
            "免费账户每月 50 万字符额度，够日常使用。"))
        if hasattr(self, "_api_lbl") and self._api_lbl:
            self._api_lbl.setText(_tr("DeepL API Key"))
        if hasattr(self, "_api_edit") and self._api_edit:
            self._api_edit.setPlaceholderText(_tr("留空则使用免费公共接口"))
        if hasattr(self, "_row_lang_lbl") and self._row_lang_lbl:
            self._row_lang_lbl.setText(_tr("翻译目标语言"))
        # 级联刷新插画区（翻译动画文字随界面语言切换）
        if hasattr(self, "illus_area") and hasattr(self.illus_area, "retranslate"):
            self.illus_area.retranslate()

    def _populate_lang_combo(self):
        try:
            from translation.languages import TRANSLATION_LANGUAGES
        except ImportError:
            TRANSLATION_LANGUAGES = {
                "ZH": "中文", "EN": "英语", "JA": "日语",
                "KO": "韩语", "FR": "法语", "DE": "德语",
            }
        try:
            from core.i18n import I18nManager
            app_lang = I18nManager.get_current_language()
        except ImportError:
            app_lang = "zh"
        default_map = {"zh": "ZH", "en": "EN", "ja": "JA"}
        saved = self._config.get_app_setting("translation_target_lang", "") or \
                default_map.get(app_lang, "ZH")

        for code, name in TRANSLATION_LANGUAGES.items():
            self._lang_combo.addItem(f"{name} ({code})", code)
        idx = self._lang_combo.findData(saved)
        if idx >= 0:
            self._lang_combo.setCurrentIndex(idx)

    def save(self):
        if hasattr(self._config, "set_deepl_api_key"):
            self._config.set_deepl_api_key(self._api_edit.text().strip())
        lang = self._lang_combo.currentData()
        if lang:
            self._config.set_app_setting("translation_target_lang", lang)


if __name__ == "__main__":
    import sys, os
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from base_page import _dev_bootstrap
    mock = _dev_bootstrap()

    from PySide6.QtWidgets import QApplication
    from wizard import WelcomeWizard

    app = QApplication(sys.argv)
    w = WelcomeWizard(mock)
    w._stack.setCurrentIndex(4)   # 跳到第5页
    w._update_nav()
    w.show()
    sys.exit(app.exec())
 