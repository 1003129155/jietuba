# -*- coding: utf-8 -*-
"""翻译设置页 — Fluent Design"""
from PySide6.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout, QLineEdit, QLabel,
    QScrollArea,
)
from PySide6.QtCore import Qt
from qfluentwidgets import (
    SwitchSettingCard, SettingCard as FSettingCard,
    FluentIcon, ComboBox, CaptionLabel,
    PushButton, HyperlinkButton,
)
from .components import SettingCardGroup, WhiteCard, adjust_button_width, theme_text_style

from translation.languages import TRANSLATION_LANGUAGES


_CARD_TITLE_STYLE = theme_text_style(14)


def create_translation_page(dialog) -> QWidget:
    """创建翻译设置页面 — Fluent Design"""
    scroll = QScrollArea()
    scroll.setWidgetResizable(True)
    scroll.setStyleSheet("QScrollArea { border: none; background: transparent; }")

    page = QWidget()
    page.setStyleSheet("background: transparent;")
    layout = QVBoxLayout(page)
    layout.setContentsMargins(0, 0, 10, 0)
    layout.setSpacing(20)

    # ════ DeepL API ════
    grp_api = SettingCardGroup(dialog.tr("DeepL API"), page)

    # API Key（卡片）
    key_card = WhiteCard(grp_api)
    key_h = QHBoxLayout(key_card)
    key_h.setContentsMargins(20, 12, 20, 12)
    key_h.setSpacing(10)

    key_lbl = QLabel(dialog.tr("DeepL API Key"), key_card)
    key_lbl.setStyleSheet(_CARD_TITLE_STYLE)
    key_lbl.setFixedWidth(100)
    key_h.addWidget(key_lbl)

    dialog.deepl_api_key_input = QLineEdit(key_card)
    dialog.deepl_api_key_input.setPlaceholderText(
        "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx:fx"
    )
    dialog.deepl_api_key_input.setText(dialog.config_manager.get_deepl_api_key())
    dialog.deepl_api_key_input.setEchoMode(QLineEdit.EchoMode.Password)
    dialog.deepl_api_key_input.setStyleSheet(dialog._get_input_style())
    key_h.addWidget(dialog.deepl_api_key_input, 1)

    dialog.show_api_key_btn = PushButton(dialog.tr("Show"), key_card)
    dialog.show_api_key_btn.setFixedHeight(32)
    adjust_button_width(dialog.show_api_key_btn, min_width=60)
    dialog.show_api_key_btn.clicked.connect(
        lambda: _toggle_api_key_visibility(dialog)
    )
    key_h.addWidget(dialog.show_api_key_btn)
    key_card.setFixedHeight(58)
    grp_api.addSettingCard(key_card)

    # Pro 开关
    pro_card = SwitchSettingCard(
        FluentIcon.CERTIFICATE,
        dialog.tr("Use DeepL Pro API"),
        dialog.tr("Enable if you have a paid DeepL subscription"),
        parent=grp_api,
    )
    pro_card.setChecked(dialog.config_manager.get_deepl_use_pro())
    dialog.deepl_pro_toggle = pro_card
    grp_api.addSettingCard(pro_card)

    layout.addWidget(grp_api)

    # ════ 翻译选项 ════
    grp_opts = SettingCardGroup(dialog.tr("Translation Options"), page)

    # 目标语言
    lang_card = FSettingCard(
        FluentIcon.LANGUAGE,
        dialog.tr("Target Language"),
        parent=grp_opts,
    )
    dialog.translation_target_combo = ComboBox(lang_card)
    dialog.translation_target_combo.setFixedWidth(180)

    lang_options = [("", dialog.tr("Auto (System)"))]
    lang_options.extend(list(TRANSLATION_LANGUAGES.items()))
    current_lang = dialog.config_manager.get_app_setting(
        "translation_target_lang", ""
    )
    current_index = 0
    for i, (code, name) in enumerate(lang_options):
        dialog.translation_target_combo.addItem(name, userData=code)
        if code == current_lang:
            current_index = i
    dialog.translation_target_combo.setCurrentIndex(current_index)

    lang_card.hBoxLayout.addWidget(
        dialog.translation_target_combo, 0, Qt.AlignmentFlag.AlignRight
    )
    lang_card.hBoxLayout.addSpacing(16)
    grp_opts.addSettingCard(lang_card)

    # 忽略换行
    split_card = SwitchSettingCard(
        FluentIcon.ALIGNMENT,
        dialog.tr("Ignore Line Breaks"),
        dialog.tr("Merge multi-line text for better translation"),
        parent=grp_opts,
    )
    split_card.setChecked(dialog.config_manager.get_translation_split_sentences())
    dialog.split_sentences_toggle = split_card
    grp_opts.addSettingCard(split_card)

    # 保留格式
    preserve_card = SwitchSettingCard(
        FluentIcon.DOCUMENT,
        dialog.tr("Preserve Formatting"),
        dialog.tr("Keep original text formatting"),
        parent=grp_opts,
    )
    preserve_card.setChecked(
        dialog.config_manager.get_translation_preserve_formatting()
    )
    dialog.preserve_formatting_toggle = preserve_card
    grp_opts.addSettingCard(preserve_card)

    layout.addWidget(grp_opts)

    # 提示
    info_label = QLabel(
        "💡 " + dialog.tr("DeepL free tier: 500,000 chars/month. Get API key at")
        + ' <a href="https://www.deepl.com/pro-api" style="color:#1976D2;">deepl.com/pro-api</a>',
        page,
    )
    info_label.setOpenExternalLinks(True)
    info_label.setStyleSheet("padding: 5px; font-size: 12px; color: #999;")
    layout.addWidget(info_label)

    layout.addStretch()
    scroll.setWidget(page)
    return scroll


def _toggle_api_key_visibility(dialog):
    """切换 API 密钥显示/隐藏"""
    if dialog.deepl_api_key_input.echoMode() == QLineEdit.EchoMode.Password:
        dialog.deepl_api_key_input.setEchoMode(QLineEdit.EchoMode.Normal)
        dialog.show_api_key_btn.setText(dialog.tr("Hide"))
        adjust_button_width(dialog.show_api_key_btn, min_width=60)
    else:
        dialog.deepl_api_key_input.setEchoMode(QLineEdit.EchoMode.Password)
        dialog.show_api_key_btn.setText(dialog.tr("Show"))
        adjust_button_width(dialog.show_api_key_btn, min_width=60)
 