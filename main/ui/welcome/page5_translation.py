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
from core.ui_scale import dialog_scaled
from ui.fluent_lite import ComboBox, LineEdit
from core import safe_event
from core.i18n import make_tr
from core.logger import log_error, log_exception, T
from ui.settings_ui import provider_fields
from core.ui_theme import set_own_style

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
        set_own_style(self, "background: transparent;")
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
        card_layout.setContentsMargins(
            dialog_scaled(22), dialog_scaled(18), dialog_scaled(22), dialog_scaled(18)
        )
        card_layout.setSpacing(dialog_scaled(14))
        layout.addWidget(self._settings_card)

        self._provider_combo = ComboBox()
        self._provider_combo.setMinimumWidth(dialog_scaled(280))
        self._provider_combo.setCursor(Qt.CursorShape.PointingHandCursor)
        from translation.service import create_default_translation_service

        # 下拉框和下面的凭据表单都从同一份 metadata 生成。以前下拉框是动态的、
        # 表单是手写的，结果 azure 在下拉框里选得到、下面却还留着上一个引擎的
        # 表单。现在两者同源，新增引擎不必再来这里补一次。
        provider_order = {"google": 0, "deepl": 1, "azure": 2, "amazon": 3}
        self._registry = create_default_translation_service(
            self._config
        ).registry
        self._provider_metadata = sorted(
            self._registry.available_providers(),
            key=lambda item: provider_order.get(item.provider_id, 99),
        )
        for metadata in self._provider_metadata:
            self._provider_combo.addItem(
                _tr(metadata.display_name), metadata.provider_id
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
        set_own_style(self._credential_stack, "background: transparent;")
        self._provider_pages = {}
        self._credential_edits = {}
        for metadata in self._provider_metadata:
            self._build_credential_page(metadata)
        card_layout.addWidget(self._credential_stack)
        self._provider_combo.currentIndexChanged.connect(
            self._on_provider_changed
        )
        self._on_provider_changed()

        # 目标语言
        self._lang_combo = ComboBox()
        self._lang_combo.setMinimumWidth(dialog_scaled(240))
        self._lang_combo.setCursor(Qt.CursorShape.PointingHandCursor)
        self._populate_lang_combo()
        row, self._row_lang_lbl = self._config_row(
            "翻译目标语言", self._lang_combo
        )
        card_layout.addWidget(row)


    def retranslate(self):
        self.title_label.setText(_tr("🌐 翻译设置").replace("🌐", "").strip())
        self.subtitle_label.setText(_tr(
            "选择翻译引擎并填写对应凭据，之后可以随时在设置中修改。"))
        if hasattr(self, "_row_provider_lbl"):
            self._row_provider_lbl.setText(_tr("翻译引擎"))
        if hasattr(self, "_row_lang_lbl") and self._row_lang_lbl:
            self._row_lang_lbl.setText(_tr("翻译目标语言"))
        for label, text in getattr(self, "_credential_labels", []):
            label.setText(_tr(text))
        for index, metadata in enumerate(getattr(self, "_provider_metadata", ())):
            # 品牌名没有翻译条目，原样返回；只有「自定义」这类名字会变
            self._provider_combo.setItemText(index, _tr(metadata.display_name))
            for field in metadata.credentials:
                edit = self._credential_edits.get(field.config_key)
                if edit is not None and field.placeholder:
                    edit.setPlaceholderText(_tr(field.placeholder))
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

    def _build_credential_page(self, metadata):
        """按 provider 自己声明的凭据字段渲染一页表单。"""
        page, form = self._credential_page()
        for field in metadata.credentials:
            edit = self._credential_edit(
                self._read_credential(field),
                _tr(field.placeholder) if field.placeholder else "",
                password=field.secret,
            )
            self._credential_edits[field.config_key] = edit
            self._add_credential_row(form, field.label, edit)

        if metadata.help_url:
            form.addRow("", self._credential_hint(
                f'<a href="{metadata.help_url}" '
                f'style="color:{ACCENT};">{_tr(metadata.help_label)}</a>'
            ))
        self._add_provider_page(metadata.provider_id, page)

    def _read_credential(self, field) -> str:
        return provider_fields.read_config(self._config, field)

    def _credential_page(self):
        page = QWidget()
        set_own_style(page, "background: transparent;")
        form = QFormLayout(page)
        form.setContentsMargins(0, dialog_scaled(2), 0, dialog_scaled(2))
        form.setHorizontalSpacing(dialog_scaled(_FORM_COLUMN_GAP))
        form.setVerticalSpacing(dialog_scaled(7))
        form.setFieldGrowthPolicy(
            QFormLayout.FieldGrowthPolicy.AllNonFixedFieldsGrow
        )
        return page, form

    @staticmethod
    def _config_row(text: str, control):
        row_widget = QWidget()
        set_own_style(row_widget, "background: transparent;")
        row = QHBoxLayout(row_widget)
        row.setContentsMargins(0, 0, 0, 0)
        row.setSpacing(dialog_scaled(_FORM_COLUMN_GAP))
        label = QLabel(_tr(text), row_widget)
        set_welcome_label_style(
            label, role="primary", font_size=14, weight=600
        )
        label.setFixedWidth(dialog_scaled(_FORM_LABEL_WIDTH))
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
        label.setFixedWidth(dialog_scaled(_FORM_LABEL_WIDTH))
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
        provider_id = self._provider_combo.currentData()
        page = self._provider_pages.get(provider_id)
        if page is None:
            # 走到这里说明下拉框里有一个没有凭据页的引擎。停在上一页会把别人的
            # 凭据显示成它的，所以宁可留空，并留一条日志指明是谁。
            log_error(
                T("翻译引擎 {provider_id} 没有凭据页", provider_id=provider_id),
                module="Welcome",
            )
            self._credential_stack.setCurrentIndex(-1)
            self._credential_stack.setFixedHeight(0)
            return
        self._credential_stack.setCurrentWidget(page)
        # Fluent inputs include 26px of content plus vertical padding and
        # borders.  Keep a small layout allowance as well so the final
        # row/focus border is never clipped at fractional DPI scales.
        self._credential_stack.setFixedHeight(
            max(dialog_scaled(68), page.sizeHint().height() + dialog_scaled(12))
        )

    def save(self):
        provider_id = self._provider_combo.currentData() or "google"
        if hasattr(self._config, "set_translation_provider"):
            self._config.set_translation_provider(provider_id)
        # 和设置页走同一套读写。以前两个页面各有一份反射实现，
        # 行为要靠人去对齐（裁不裁空白、异常怎么吞），对不齐也不会报错。
        provider_fields.save_from(
            self._config,
            provider_fields.all_fields(self._registry),
            self._credential_edits,
        )
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
    w._stack.setCurrentIndex(4)   # 跳到翻译设置页
    w._update_nav()
    w.show()
    sys.exit(app.exec())
 
