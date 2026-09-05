# -*- coding: utf-8 -*-
"""
翻译设置页

上半部：文字翻译流向动画
下半部：翻译引擎选择 + 对应凭据 + 目标语言 + 快捷键
"""

from PySide6.QtWidgets import (
    QFormLayout, QVBoxLayout, QLabel, QLineEdit, QWidget, QHBoxLayout,
    QStackedWidget, QSizePolicy,
)
from PySide6.QtCore import Qt, QTimer
from PySide6.QtGui import QPainter, QColor, QFont, QPen
from ui.fluent_lite import ComboBox, LineEdit
from core import safe_event
from core.i18n import make_tr

if __package__:
    from .base_page import (
        BasePage, IllustrationArea, ACCENT, welcome_theme, set_welcome_label_style,
    )
else:
    import sys, os
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from base_page import (
        BasePage, IllustrationArea, ACCENT, welcome_theme, set_welcome_label_style,
    )


_tr = make_tr("WelcomeWizard")

_FORM_LABEL_WIDTH = 96
_FORM_COLUMN_GAP = 12


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
            from core.logger import log_exception, T
            log_exception(e, T("获取当前语言"))
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
        theme = welcome_theme()

        card_w = min(w - 40, 280)
        card_h = 58
        cx = (w - card_w) // 2
        gap = 14
        cy_src = h // 2 - card_h - gap // 2
        cy_dst = h // 2 + gap // 2

        # ── 卡片背景 ──
        p.setPen(QPen(QColor(theme.border_strong), 1))
        p.setBrush(QColor(theme.panel))
        p.drawRoundedRect(cx, cy_src, card_w, card_h, 8, 8)
        p.drawRoundedRect(cx, cy_dst, card_w, card_h, 8, 8)

        # ── 语言标签 ──
        tag_font = QFont("Microsoft YaHei", 8)
        p.setFont(tag_font)
        p.setPen(QColor(theme.accent))
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
            color = QColor(theme.text)
            color.setAlpha(src_a)
            p.setPen(color)
            p.drawText(cx + 10, cy_src + 20, card_w - 20, card_h - 24,
                       Qt.AlignmentFlag.AlignVCenter, self._src_text)

        if dst_a > 0:
            color = QColor(theme.text)
            color.setAlpha(dst_a)
            p.setPen(color)
            p.drawText(cx + 10, cy_dst + 20, card_w - 20, card_h - 24,
                       Qt.AlignmentFlag.AlignVCenter, self._dst_text)

        # ── 箭头（翻译进行中时高亮）──
        arrow_a = 200 if self._phase in (1, 2, 3) else 80
        arrow = QColor(theme.accent)
        arrow.setAlpha(arrow_a)
        pen = QPen(arrow, 2)
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
    """翻译 API 与全局快捷键设置"""

    def __init__(self, config_manager, parent=None):
        self._config = config_manager
        super().__init__(
            title=_tr("🌐 翻译设置").replace("🌐", "").strip(),
            subtitle=_tr(
                "选择翻译引擎并填写对应凭据，之后可以随时在设置中修改。"
            ),
            parent=parent,
        )
        # This page is configuration-heavy; give the form the full body width.
        self.illus_area.hide()

    def _create_illustration(self):
        return _TranslateIllus(self)

    def _build_controls(self, layout: QVBoxLayout):
        self._settings_card = QWidget()
        self._settings_card.setObjectName("SettingRow")
        self._settings_card.setProperty("welcomeSettingRow", True)
        card_layout = QVBoxLayout(self._settings_card)
        card_layout.setContentsMargins(22, 18, 22, 18)
        card_layout.setSpacing(14)
        layout.addWidget(self._settings_card)

        self._provider_combo = ComboBox()
        self._provider_combo.setMinimumWidth(280)
        self._provider_combo.setCursor(Qt.CursorShape.PointingHandCursor)
        from translation.service import create_default_translation_service

        provider_order = {"google": 0, "deepl": 1, "amazon": 2}
        providers = create_default_translation_service(
            self._config
        ).registry.available_providers()
        for metadata in sorted(
            providers,
            key=lambda item: provider_order.get(item.provider_id, 99),
        ):
            self._provider_combo.addItem(
                metadata.display_name, metadata.provider_id
            )
        saved_provider = (
            self._config.get_translation_provider()
            if hasattr(self._config, "get_translation_provider")
            else "google"
        )
        provider_index = self._provider_combo.findData(
            saved_provider or "google"
        )
        self._provider_combo.setCurrentIndex(
            provider_index if provider_index >= 0 else 0
        )
        provider_row, self._row_provider_lbl = self._config_row(
            "翻译引擎", self._provider_combo
        )
        card_layout.addWidget(provider_row)

        self._credential_stack = QStackedWidget()
        self._credential_stack.setStyleSheet("background: transparent;")
        self._provider_pages = {}
        self._build_google_credentials()
        self._build_deepl_credentials()
        self._build_amazon_credentials()
        card_layout.addWidget(self._credential_stack)
        self._provider_combo.currentIndexChanged.connect(
            self._on_provider_changed
        )
        self._on_provider_changed()

        # 目标语言
        self._lang_combo = ComboBox()
        self._lang_combo.setMinimumWidth(240)
        self._lang_combo.setCursor(Qt.CursorShape.PointingHandCursor)
        self._populate_lang_combo()
        row, self._row_lang_lbl = self._config_row(
            "翻译目标语言", self._lang_combo
        )
        card_layout.addWidget(row)

        # 全局翻译快捷键（主 + 备用）
        if __package__:
            from ..hotkey_edit import HotkeyEdit
        else:
            import sys, os
            sys.path.insert(
                0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
            )
            from hotkey_edit import HotkeyEdit

        hotkey_controls = QWidget()
        hotkey_controls.setStyleSheet("background: transparent;")
        hotkey_layout = QHBoxLayout(hotkey_controls)
        hotkey_layout.setContentsMargins(0, 0, 0, 0)
        hotkey_layout.setSpacing(8)

        self._hotkey = HotkeyEdit()
        self._hotkey.setFixedHeight(36)
        self._hotkey.setText(self._config.get_translation_hotkey())
        hotkey_layout.addWidget(self._hotkey, 1)

        self._hotkey2 = HotkeyEdit()
        self._hotkey2.setFixedHeight(36)
        self._hotkey2.setText(self._config.get_translation_hotkey_2())
        hotkey_layout.addWidget(self._hotkey2, 1)

        # 标题单独占一行，输入区仍与上方所有表单控件共用同一基线。
        # 这样较长的日文标题不会挤窄或推歪两个快捷键输入框。
        hotkey_section = QWidget()
        hotkey_section.setStyleSheet("background: transparent;")
        hotkey_section_layout = QVBoxLayout(hotkey_section)
        hotkey_section_layout.setContentsMargins(0, 0, 0, 0)
        hotkey_section_layout.setSpacing(7)

        self._row_hotkey_lbl = QLabel(_tr("快捷键（最多设置两个）"))
        set_welcome_label_style(
            self._row_hotkey_lbl, role="primary", font_size=14, weight=600
        )
        hotkey_section_layout.addWidget(self._row_hotkey_lbl)

        hotkey_row = QHBoxLayout()
        hotkey_row.setContentsMargins(0, 0, 0, 0)
        hotkey_row.setSpacing(0)
        hotkey_row.addSpacing(_FORM_LABEL_WIDTH + _FORM_COLUMN_GAP)
        hotkey_row.addWidget(hotkey_controls, 1)
        hotkey_section_layout.addLayout(hotkey_row)
        card_layout.addWidget(hotkey_section)

    def retranslate(self):
        self.title_label.setText(_tr("🌐 翻译设置").replace("🌐", "").strip())
        self.subtitle_label.setText(_tr(
            "选择翻译引擎并填写对应凭据，之后可以随时在设置中修改。"))
        if hasattr(self, "_row_provider_lbl"):
            self._row_provider_lbl.setText(_tr("翻译引擎"))
        if hasattr(self, "_row_lang_lbl") and self._row_lang_lbl:
            self._row_lang_lbl.setText(_tr("翻译目标语言"))
        if hasattr(self, "_row_hotkey_lbl") and self._row_hotkey_lbl:
            self._row_hotkey_lbl.setText(_tr("快捷键（最多设置两个）"))
        for label, text in getattr(self, "_credential_labels", []):
            label.setText(_tr(text))
        if hasattr(self, "_google_key_edit"):
            self._google_key_edit.setPlaceholderText(_tr("Google API Key"))
        if hasattr(self, "_deepl_key_edit"):
            self._deepl_key_edit.setPlaceholderText(_tr("DeepL API Key"))
        if hasattr(self, "_amazon_secret_edit"):
            self._amazon_secret_edit.setPlaceholderText(
                _tr("Secret Access Key")
            )
        if hasattr(self, "_amazon_token_edit"):
            self._amazon_token_edit.setPlaceholderText(
                _tr("可选，临时凭据使用")
            )
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

    def _build_google_credentials(self):
        page, form = self._credential_page()
        self._google_key_edit = self._credential_edit(
            self._config.get_google_translate_api_key()
            if hasattr(self._config, "get_google_translate_api_key")
            else "",
            "Google API Key",
            password=True,
        )
        self._add_credential_row(form, "Google API Key", self._google_key_edit)
        hint = self._credential_hint(
            f'<a href="https://console.cloud.google.com/apis/credentials" '
            f'style="color:{ACCENT};">Google Cloud Console</a>'
        )
        form.addRow("", hint)
        self._add_provider_page("google", page)

    def _build_deepl_credentials(self):
        page, form = self._credential_page()
        self._deepl_key_edit = self._credential_edit(
            self._config.get_deepl_api_key()
            if hasattr(self._config, "get_deepl_api_key")
            else "",
            "DeepL API Key",
            password=True,
        )
        self._add_credential_row(form, "DeepL API Key", self._deepl_key_edit)
        hint = self._credential_hint(
            f'<a href="https://www.deepl.com/pro-api" '
            f'style="color:{ACCENT};">deepl.com/pro-api</a>'
        )
        form.addRow("", hint)
        self._add_provider_page("deepl", page)

    def _build_amazon_credentials(self):
        page, form = self._credential_page()
        self._amazon_region_edit = self._credential_edit(
            self._config.get_amazon_translate_region()
            if hasattr(self._config, "get_amazon_translate_region")
            else "us-west-2",
            "us-west-2",
        )
        self._amazon_access_edit = self._credential_edit(
            self._config.get_amazon_translate_access_key_id()
            if hasattr(self._config, "get_amazon_translate_access_key_id")
            else "",
            "AKIA...",
        )
        self._amazon_secret_edit = self._credential_edit(
            self._config.get_amazon_translate_secret_access_key()
            if hasattr(
                self._config, "get_amazon_translate_secret_access_key"
            )
            else "",
            "Secret Access Key",
            password=True,
        )
        self._amazon_token_edit = self._credential_edit(
            self._config.get_amazon_translate_session_token()
            if hasattr(self._config, "get_amazon_translate_session_token")
            else "",
            "可选，临时凭据使用",
            password=True,
        )
        self._add_credential_row(form, "AWS 区域", self._amazon_region_edit)
        self._add_credential_row(
            form, "Access Key ID", self._amazon_access_edit
        )
        self._add_credential_row(
            form, "Secret Access Key", self._amazon_secret_edit
        )
        self._add_credential_row(
            form, "Session Token", self._amazon_token_edit
        )
        self._add_provider_page("amazon", page)

    def _credential_page(self):
        page = QWidget()
        page.setStyleSheet("background: transparent;")
        form = QFormLayout(page)
        form.setContentsMargins(0, 2, 0, 2)
        form.setHorizontalSpacing(_FORM_COLUMN_GAP)
        form.setVerticalSpacing(7)
        form.setFieldGrowthPolicy(
            QFormLayout.FieldGrowthPolicy.AllNonFixedFieldsGrow
        )
        return page, form

    @staticmethod
    def _config_row(text: str, control):
        row_widget = QWidget()
        row_widget.setStyleSheet("background: transparent;")
        row = QHBoxLayout(row_widget)
        row.setContentsMargins(0, 0, 0, 0)
        row.setSpacing(_FORM_COLUMN_GAP)
        label = QLabel(_tr(text), row_widget)
        set_welcome_label_style(
            label, role="primary", font_size=14, weight=600
        )
        label.setFixedWidth(_FORM_LABEL_WIDTH)
        control.setSizePolicy(
            QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed
        )
        row.addWidget(label)
        row.addWidget(control, 1)
        return row_widget, label

    def _credential_edit(
        self, value: str, placeholder: str, *, password: bool = False
    ):
        edit = LineEdit()
        edit.setText(value or "")
        edit.setPlaceholderText(placeholder)
        if password:
            edit.setEchoMode(QLineEdit.EchoMode.Password)
        return edit

    def _add_credential_row(self, form, text: str, edit):
        label = QLabel(_tr(text))
        set_welcome_label_style(
            label, role="primary", font_size=12, weight=600
        )
        label.setFixedWidth(_FORM_LABEL_WIDTH)
        self._credential_labels = getattr(
            self, "_credential_labels", []
        )
        self._credential_labels.append((label, text))
        form.addRow(label, edit)

    @staticmethod
    def _credential_hint(text: str):
        label = QLabel(text)
        label.setOpenExternalLinks(True)
        set_welcome_label_style(
            label, role="muted", font_size=11, weight=400
        )
        return label

    def _add_provider_page(self, provider_id: str, page: QWidget):
        self._provider_pages[provider_id] = page
        self._credential_stack.addWidget(page)

    def _on_provider_changed(self, *_args):
        page = self._provider_pages.get(
            self._provider_combo.currentData()
        )
        if page is not None:
            self._credential_stack.setCurrentWidget(page)
            # Fluent inputs include 26px of content plus vertical padding and
            # borders.  Keep a small layout allowance as well so the final
            # row/focus border is never clipped at fractional DPI scales.
            self._credential_stack.setFixedHeight(
                max(68, page.sizeHint().height() + 12)
            )

    def save(self):
        provider_id = self._provider_combo.currentData() or "google"
        if hasattr(self._config, "set_translation_provider"):
            self._config.set_translation_provider(provider_id)
        if hasattr(self._config, "set_google_translate_api_key"):
            self._config.set_google_translate_api_key(
                self._google_key_edit.text().strip()
            )
        if hasattr(self._config, "set_deepl_api_key"):
            self._config.set_deepl_api_key(
                self._deepl_key_edit.text().strip()
            )
        if hasattr(self._config, "set_amazon_translate_region"):
            self._config.set_amazon_translate_region(
                self._amazon_region_edit.text().strip()
            )
            self._config.set_amazon_translate_access_key_id(
                self._amazon_access_edit.text().strip()
            )
            self._config.set_amazon_translate_secret_access_key(
                self._amazon_secret_edit.text().strip()
            )
            self._config.set_amazon_translate_session_token(
                self._amazon_token_edit.text().strip()
            )
        lang = self._lang_combo.currentData()
        if lang:
            self._config.set_app_setting("translation_target_lang", lang)
        self._config.set_translation_hotkey(self._hotkey.text().strip())
        self._config.set_translation_hotkey_2(self._hotkey2.text().strip())


if __name__ == "__main__":
    import sys, os
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from base_page import _dev_bootstrap
    mock = _dev_bootstrap()

    from PySide6.QtWidgets import QApplication
    from wizard import WelcomeWizard

    app = QApplication(sys.argv)
    w = WelcomeWizard(mock)
    w._stack.setCurrentIndex(3)   # 跳到翻译设置页
    w._update_nav()
    w.show()
    sys.exit(app.exec())
 
